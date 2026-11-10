#include "native_history_fixture.hpp"
#include "test_cases.hpp"
#include "sealed_internal.hpp"
#include <syrnck/sealed.hpp>
#include <iostream>
using namespace native_test;
using namespace syrnck::sealed_detail;
namespace {
SecretKey master() {
  Key key{};
  key.fill(0x71);
  return SecretKey::from_bytes(key);
}
std::map<std::string, std::string> physical(const Store &store) {
  std::map<std::string, std::string> out;
  for (const auto &item : fs::recursive_directory_iterator(store.root()))
    if (item.is_regular_file() && item.path().filename() != "store.lock")
      out.emplace(utf8(item.path().lexically_relative(store.root())), hash(read_bytes(item.path())));
  return out;
}
void scenario(const fs::path &path, bool encrypted, unsigned layout) {
  std::optional<Key> at_rest;
  if (encrypted) {
    at_rest.emplace();
    at_rest->fill(0x21);
  }
  Fixture source(path / "source", at_rest);
  Store wire(path / "wire", {}, true), destination(path / "destination", at_rest, true);
  auto key = master();
  Json files = Json::object();
  for (unsigned i = 0; i < 150; ++i)
    files["Content/PrivateAsset" + std::to_string(i) + ".uasset"] =
        source.file("PRIVATE-PAYLOAD-" + std::to_string(i));
  const auto first = source.revision(files);
  source.store.set_ref("heads/main", {Kind::revision, first});
  source.pack(layout);
  const auto source_before = source.physical();
  const Refs roots{{"heads/private-main", {Kind::revision, first}}};
  auto sealed = seal_snapshot(source.store, wire, key, "team", roots);
  const auto first_wire = sealed["snapshot"].get<std::string>();
  require(sealed["sealed_objects"].get<std::size_t>() > 256 &&
              sealed["index_nodes_written"].get<std::size_t>() > 1,
          "sealed snapshot did not split its opaque index");
  require(source.physical() == source_before, "sealing changed the plaintext source");
  // Inspect every logical wire object, not just compressed/encrypted disk bytes.
  for (const auto &object : wire.objects()) {
    const auto data = text(wire.get(object));
    require(data.find("PRIVATE-PAYLOAD-") == std::string::npos &&
                data.find("PrivateAsset") == std::string::npos &&
                data.find("heads/private-main") == std::string::npos && data.find(first) == std::string::npos,
            "wire objects exposed source payloads, paths or revision identifiers");
  }
  if (layout)
    wire.pack();
  if (layout == 2)
    wire.pack({.compact = true});
  const auto wire_before = physical(wire);
  auto verified = inspect_sealed_snapshot(wire, key, first_wire, "team");
  require(verified["roots"] == encode_refs(roots) && physical(wire) == wire_before,
          "sealed inspection changed ciphertext or lost private references");
  auto unchanged = seal_snapshot(source.store, wire, key, "team", roots, first_wire);
  require(unchanged["state"] == "unchanged" && unchanged["snapshot"] == first_wire &&
              physical(wire) == wire_before,
          "unchanged sealing rewrote random envelopes or index nodes");
  auto opened =
      open_sealed_snapshot(wire, destination, key, first_wire, "team", "sealed/checkpoints/origin-team");
  require(opened["objects"] == sealed["objects"] && !destination.refs().contains("heads/private-main"),
          "opening replaced application branches or lost objects");
  require(destination.metadata(Kind::revision, first) == source.store.metadata(Kind::revision, first),
          "sealed roundtrip changed the original revision");
  destination.gc(true);
  destination.export_revision(first, path / "restored");
  require(text(read_bytes(path / "restored/Content/PrivateAsset0.uasset")) == "PRIVATE-PAYLOAD-0",
          "sealed retention did not preserve original blocks through GC");
  // A changed file creates only its new objects and the affected Merkle path.
  files["Content/PrivateAsset0.uasset"] = source.file("PRIVATE-PAYLOAD-UPDATED");
  const auto second = source.revision(files, Json::array({first}));
  const auto updated = seal_snapshot(source.store, wire, key, "team",
                                     {{"heads/private-main", {Kind::revision, second}}}, first_wire);
  const auto second_wire = updated["snapshot"].get<std::string>();
  require(updated["sealed_objects"] == 3 && updated["reused_objects"] == sealed["objects"] &&
              updated["index_nodes_written"].get<std::size_t>() <= 6,
          "sealed update re-encrypted unchanged blocks or rewrote the whole index");
  const auto before_refs = destination.refs();
  fails([&] {
    open_sealed_snapshot(wire, destination, key, second_wire, "wrong-channel",
                         "sealed/checkpoints/origin-team");
  });
  require(destination.refs() == before_refs, "wrong-channel opening changed refs");
  open_sealed_snapshot(wire, destination, key, second_wire, "team", "sealed/checkpoints/origin-team");
  const auto after_refs = destination.refs();
  fails([&] {
    open_sealed_snapshot(wire, destination, key, first_wire, "team", "sealed/checkpoints/origin-team");
  });
  require(destination.refs() == after_refs, "sealed rollback changed the accepted checkpoint");
  // Explicit historical inspection/restoration has a separate local checkpoint.
  open_sealed_snapshot(wire, destination, key, first_wire, "team", "sealed/checkpoints/historical-team");
  destination.gc(true);
  wire.gc(true);
  destination.fsck();
  wire.fsck();
  require(inspect_sealed_snapshot(wire, key, first_wire, "team")["roots"] == encode_refs(roots),
          "new sealed snapshots discarded old encrypted history");
  if (!encrypted && !layout) {
    SealedLimits tiny;
    tiny.max_index_cache_bytes = 1024;
    require(inspect_sealed_snapshot(wire, key, second_wire, "team", tiny)["objects"] == updated["objects"],
            "sealed index traversal depended on keeping all nodes cached");
  }
}
void refusals(const fs::path &path) {
  Fixture source(path / "source");
  Store wire(path / "wire", {}, true), destination(path / "destination", {}, true);
  auto key = master();
  const auto file = source.file("private input");
  const auto revision = source.revision(Json{{"asset", file}});
  const Refs roots{{"heads/main", {Kind::revision, revision}}};
  const auto original_source = source.physical(), original_wire = physical(wire);
  SealedLimits limits;
  limits.max_objects = 1;
  fails([&] { seal_snapshot(source.store, wire, key, "main", roots, {}, limits); });
  limits = {};
  limits.max_work = 1;
  fails([&] { seal_snapshot(source.store, wire, key, "main", roots, {}, limits); });
  limits = {};
  limits.max_new_index_bytes = 1024;
  Refs many;
  for (unsigned i = 0; i < 7; ++i)
    many.emplace(
        "root/" + std::to_string(i),
        ObjectKey{Kind::manifest, source.file("new-" + std::to_string(i))["manifest"].get<std::string>()});
  fails([&] { seal_snapshot(source.store, wire, key, "main", many, {}, limits); });
  require(physical(wire) == original_wire, "failed sealed preflight wrote ciphertext");
  fails([&] { seal_snapshot(source.store, source.store, key, "main", roots); });
  std::stop_source stop;
  stop.request_stop();
  fails([&] { seal_snapshot(source.store, wire, key, "main", roots, {}, {}, {stop.get_token()}); });
  require(physical(wire) == original_wire, "cancelled sealed preflight wrote ciphertext");
  const auto result = seal_snapshot(source.store, wire, key, "main", roots);
  const auto root = result["snapshot"].get<std::string>();
  const auto untouched = physical(destination);
  const auto wrong = SecretKey::generate();
  fails([&] { open_sealed_snapshot(wire, destination, wrong, root, "main", "sealed/checkpoints/remote"); });
  require(physical(destination) == untouched, "wrong sealed key wrote plaintext");
  auto wrapper = wire.metadata(Kind::revision, root);
  wrapper["timestamp"] = wrapper["timestamp"].get<std::uint64_t>() + 1;
  const auto forged = wire.put(Kind::revision, bytes(wrapper.dump())).first;
  fails([&] { inspect_sealed_snapshot(wire, key, forged.id, "main"); });
  wrapper = wire.metadata(Kind::revision, root);
  wrapper["parents"] = Json::array();
  const auto detached = wire.put(Kind::revision, bytes(wrapper.dump())).first;
  fails([&] { inspect_sealed_snapshot(wire, key, detached.id, "main"); });
  require(physical(destination) == untouched, "forged sealed metadata wrote plaintext");
  fails([&] { open_sealed_snapshot(wire, destination, key, root, "main", "heads/main"); });
  const Cipher cipher(key);
  const auto frame = cipher.seal("object", std::string(64, '1'), bytes("payload"));
  fails([&] { cipher.open("object", std::string(64, '2'), frame, max_object_size); });
  fails([&] { cipher.open("roots", std::string(64, '1'), frame, max_object_size); });
  for (const auto offset :
       {std::size_t{0}, std::size_t{8}, std::size_t{24}, std::size_t{48}, frame.size() - 1}) {
    auto changed = frame;
    changed[offset] ^= 1;
    fails([&] { cipher.open("object", std::string(64, '1'), changed, max_object_size); });
  }
  auto twice = cipher.seal("object", std::string(64, '1'), bytes("payload"));
  require(twice != frame &&
              text(cipher.open("object", std::string(64, '1'), twice, max_object_size)) == "payload",
          "sealed encryption reused a nonce or changed plaintext");
  // The source fixture gained only the deliberately added inputs above.
  require(source.store.refs().empty() && !original_source.empty(), "sealed calls changed source references");
}
void maximum_chunk(const fs::path &path) {
  Fixture source(path / "source");
  Store wire(path / "wire", {}, true), destination(path / "destination", {}, true);
  auto key = master();
  const auto payload = std::string(max_object_size, 'z');
  const auto file = source.file(payload, false, static_cast<std::uint32_t>(max_object_size));
  const auto snapshot = seal_snapshot(source.store, wire, key, "large",
                                      {{"file", {Kind::manifest, file["manifest"].get<std::string>()}}});
  open_sealed_snapshot(wire, destination, key, snapshot["snapshot"].get<std::string>(), "large",
                       "sealed/checkpoints/large");
  std::ostringstream out;
  destination.reconstruct(file["manifest"].get<std::string>(), out);
  require(out.str() == payload, "sealed 16 MiB object did not survive envelope overhead and wire chunking");
}
void independent_vectors() {
  // Generated with the pure Rust BLAKE3 1.8.7 implementation and RustCrypto
  // chacha20poly1305 0.10.1, independently of the native BLAKE3/Monocypher code.
  // Public fixture: master=32*0x71, nonce=24*0x22, plaintext="payload".
  const auto key = master();
  const Cipher cipher(key);
  require(cipher.realm == "d9ec988a39dfbc52de95293fb84f3f03", "sealed realm reference vector mismatch");
  const auto address = cipher.address({Kind::chunk, std::string(64, 'a')});
  require(address == "988adc604fb44a543c799fdf2120b81d3702ebc5788906e9be7d17c42a3900e2",
          "sealed address reference vector mismatch");
  const auto object = unhex("5359525345413031d9ec988a39dfbc52de95293fb84f3f03"
                            "222222222222222222222222222222222222222222222222"
                            "f11393349238ed801601ba08b9e76c5433f774168ff8e4");
  const auto roots = unhex("5359525345413031d9ec988a39dfbc52de95293fb84f3f03"
                           "222222222222222222222222222222222222222222222222"
                           "57931286691eab3bd484b2013f0c4223fb5d4826bf49e8");
  require(text(cipher.open("object", address, object, 7)) == "payload" &&
              text(cipher.open("roots", "team", roots, 7)) == "payload",
          "sealed AEAD reference vector mismatch");
  fails([&] { cipher.open("object", address, roots, 7); });
  fails([&] { cipher.open("roots", "team", object, 7); });
}
void block_reuse(const fs::path &path) {
  Key at_rest;
  at_rest.fill(0x12);
  Fixture source(path / "source", at_rest);
  at_rest.fill(0x13);
  Store wire(path / "wire", at_rest, true);
  at_rest.fill(0x14);
  Store destination(path / "destination", at_rest, true);
  Key seed;
  seed.fill(0x45);
  source.store.use_native_identity({"Carrier author", "carrier@example.invalid", 0},
                                   SigningKey::from_seed(seed));
  const auto key = master();
  auto payload =
      std::string(4096, 'a') + std::string(4096, 'b') + std::string(4096, 'c') + std::string(4096, 'd');
  const auto first_file = source.file(payload, false, 4096);
  const auto first = source.revision(Json{{"asset.uasset", first_file}});
  const auto original =
      seal_snapshot(source.store, wire, key, "blocks", {{"heads/main", {Kind::revision, first}}});
  wire.pack();
  const auto old_id = original["snapshot"].get<std::string>();
  const auto carrier = wire.metadata(Kind::revision, old_id);
  require(carrier["version"] == 2 && carrier["signature"].is_object(),
          "sealed preparation omitted required carrier signatures");
  // Removing a valid public signer by replacing its wrapper with legacy v1
  // must fail against the authenticated public-identity commitment.
  const auto stripped = wire.put(Kind::revision, bytes(Json{{"version", 1},
                                                            {"parents", carrier["parents"]},
                                                            {"message", carrier["message"]},
                                                            {"timestamp", carrier["timestamp"]},
                                                            {"files", carrier["files"]}}
                                                           .dump()))
                            .first;
  fails([&] { inspect_sealed_snapshot(wire, key, stripped.id, "blocks"); });
  payload.replace(4096, 4096, std::string(4096, 'e'));
  const auto second_file = source.file(payload, false, 4096);
  const auto second = source.revision(Json{{"asset.uasset", second_file}}, Json::array({first}));
  const auto updated =
      seal_snapshot(source.store, wire, key, "blocks", {{"heads/main", {Kind::revision, second}}}, old_id);
  require(updated["sealed_objects"] == 3 && updated["reused_objects"] == original["objects"],
          "editing one block re-encrypted unchanged blocks of the same file");
  const SealedLimits limits;
  Budget budget{limits, {}};
  Index index(wire, budget);
  const Cipher cipher(key);
  const auto manifest = source.store.metadata(Kind::manifest, first_file["manifest"].get<std::string>());
  for (const auto &block : manifest["blocks"]) {
    const auto address = cipher.address({Kind::chunk, block["id"].get<std::string>()});
    require(index.find(original["index"].get<std::string>(), address) ==
                index.find(updated["index"].get<std::string>(), address),
            "sealed update replaced an unchanged ciphertext envelope");
  }
  open_sealed_snapshot(wire, destination, key, updated["snapshot"].get<std::string>(), "blocks",
                       "sealed/checkpoints/blocks");
  std::ostringstream restored;
  destination.reconstruct(second_file["manifest"].get<std::string>(), restored);
  require(restored.str() == payload, "independently encrypted Stores lost sealed block reconstruction");
  destination.gc(true);
  destination.fsck();
  wire.fsck();
}
} // namespace
int main(int argc, char **argv) {
  Temporary temp;
  try {
    fixture_test::TestCases cases(fixture_test::CaseSelection::parse(argc, argv));
    cases.add("independent-vectors", [](const fs::path &) { independent_vectors(); });
    cases.add("block-reuse", [](const fs::path &path) { block_reuse(path); });
    for (bool encrypted : {false, true})
      for (unsigned layout = 0; layout < 3; ++layout)
        cases.add("layout-" + std::to_string(encrypted) + '-' + std::to_string(layout),
                  [encrypted, layout](const fs::path &path) { scenario(path, encrypted, layout); });
    cases.add("refusals", [](const fs::path &path) { refusals(path); });
    cases.add("maximum-chunk", [](const fs::path &path) { maximum_chunk(path); });
    return cases.run(temp.path);
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
