#include "native_history_fixture.hpp"
#include "pack_storage.hpp"
#include <syrnck/backup.hpp>
#include <iostream>
using namespace native_test;
namespace {
bool armed = false, tracing = false;
std::size_t seen = 0, wanted = 0;
std::optional<Key> key(bool encrypted, unsigned value) {
  if (!encrypted) return {};
  Key result; result.fill(static_cast<std::uint8_t>(value)); return result;
}
fs::path loose(const Store &store, const ObjectKey &object) {
  return store.root() / "objects" / kind_name(object.kind) / object.id.substr(0, 2) / object.id;
}
void damage(const Store &store, const ObjectKey &object, unsigned layout) {
  if (!layout) { atomic_write(loose(store, object), bytes("corrupt original envelope"), true); return; }
  const auto config = parse_json(read_bytes(store.root() / "config.json"));
  PackStorage packs(store.root()); packs.load(config.contains("packs") ? config["packs"] : config["pack"]);
  const auto position = packs.locate(object); require(position.has_value(), "crash fixture object not packed");
  const auto name = packs.catalog()[position->segment]["name"].get<std::string>();
  const auto index = read_bytes(store.root() / "packs" / ("pack-" + name + ".syi"));
  const auto record = PackIndex::record(std::span(index).subspan(PackIndex::header_size), position->ordinal);
  const auto path = store.root() / "packs" / ("pack-" + name + ".syp");
  auto data = read_bytes(path); data.at(static_cast<std::size_t>(record.offset)) ^= 31; atomic_write(path, data, true);
}
void prepare(const fs::path &root, bool encrypted, unsigned layout) {
  Fixture f(root / "source", key(encrypted, 57));
  const auto old = f.file("archived original bytes"), current = f.file("current revision bytes");
  const auto first = f.revision(Json{{"old.bin", old}}); f.install(first);
  f.store.set_ref("archive/amend/" + first, {Kind::revision, first});
  const auto last = f.revision(Json{{"current.bin", current}}); f.install(last);
  const auto orphan = f.store.put(Kind::chunk, bytes("valid local object outside backup roots")).first;
  f.pack(layout);
  const auto missing = f.store.put(Kind::chunk, bytes("missing pinned bytes")).first;
  f.store.set_ref("fixture/missing", missing);
  create_backup(f.store, root / "backup", key(encrypted, 65));
  const auto manifest = old["manifest"].get<std::string>();
  const ObjectKey chunk{Kind::chunk, f.store.metadata(Kind::manifest, manifest)["blocks"][0]["id"].get<std::string>()};
  damage(f.store, chunk, layout);
  if (layout) {
    atomic_write(loose(f.store, chunk), bytes("corrupt loose shadow"), false);
    damage(f.store, {Kind::revision, first}, layout);
  }
  fs::remove(loose(f.store, missing));
  atomic_write(root / "fixture.json", bytes(Json{{"encrypted", encrypted}, {"layout", layout},
      {"refs", encode_refs(f.store.refs())}, {"old", old["manifest"]}, {"current", current["manifest"]},
      {"orphan", key_json(orphan)}, {"missing", key_json(missing)}}.dump()), false);
}
Json verify(Store &source, Store &backup, const Json &info, bool complete) {
  require(encode_refs(source.refs()) == info["refs"], "storage repair crash changed roots");
  const auto plan = plan_storage_repair(source, backup);
  require(plan["ready"] == true, "storage repair crash lost recoverable data");
  require(text(source.get(parse_key(info["orphan"]))) == "valid local object outside backup roots",
          "storage repair crash lost valid local bytes absent from backup");
  if (complete) {
    require(plan["missing_count"] == 0 && plan["corrupt_count"] == 0, "resumed storage repair is incomplete");
    source.fsck();
    for (const auto name : {"old", "current"}) {
      std::ostringstream output; source.reconstruct(info[name].get<std::string>(), output);
      require(output.str() == (std::string_view(name) == "old" ? "archived original bytes" : "current revision bytes"),
              "storage repair crash changed version bytes");
    }
    require(text(source.get(parse_key(info["missing"]))) == "missing pinned bytes", "resumed storage repair omitted a pin");
  }
  return Json{{"verified", true}, {"missing", plan["missing_count"]}, {"corrupt", plan["corrupt_count"]}};
}
}
namespace syrnck::testing {
void io_checkpoint(std::string_view phase, const fs::path &target) {
  if (!armed) return;
  ++seen;
  const auto extension = target.extension();
  const auto kind = extension == ".syp" ? "pack" : extension == ".syi" ? "index" :
      target.filename() == "config.json" ? "config" : utf8(target.parent_path().parent_path().filename());
  if (tracing || seen == wanted) {
    std::cout << Json{{"index", seen}, {"phase", phase}, {"kind", kind}}.dump() << std::endl;
    if (!tracing) {
      armed = false;
      std::string line; require(static_cast<bool>(std::getline(std::cin, line)) && line == "continue", "storage repair checkpoint closed");
    }
  }
}
}
int main(int argc, char **argv) {
  try {
    require(argc >= 3, "storage repair probe requires mode and root");
    const std::string mode = argv[1]; const auto root = path_from_utf8(argv[2]);
    if (mode == "prepare") { require(argc == 5, "storage repair prepare requires encryption and layout"); prepare(root, std::string_view(argv[3]) == "encrypted", static_cast<unsigned>(std::stoul(argv[4]))); return 0; }
    const auto info = parse_json(read_bytes(root / "fixture.json"));
    const auto encrypted = info["encrypted"].get<bool>();
    Store source(root / "source/.syrnck", key(encrypted, 57)); Store backup(root / "backup", key(encrypted, 65));
    if (mode == "trace" || mode == "run" || mode == "resume") {
      const auto plan = plan_storage_repair(source, backup);
      if (mode != "resume") {
        tracing = mode == "trace";
        if (!tracing) { require(argc == 4, "storage repair run requires checkpoint"); wanted = static_cast<std::size_t>(std::stoull(argv[3])); }
        armed = true;
      }
      repair_storage(source, backup, plan["plan"].get<std::string>());
      require(tracing || mode == "resume" || !armed, "storage repair checkpoint not reached"); armed = false;
    } else require(mode == "verify", "unknown storage repair probe mode");
    std::cout << verify(source, backup, info, mode != "verify").dump() << '\n';
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
