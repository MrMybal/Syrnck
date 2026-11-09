#include "pack_storage.hpp"
#include "test_cases.hpp"
#include <iostream>
#include <sstream>
using namespace syrnck;
namespace {
void check(bool value) {
  require(value, "streaming audit test failed");
}
template <class Function> void fails(Function function) {
  bool failed = false;
  try {
    function();
  } catch (const std::exception &) {
    failed = true;
  }
  check(failed);
}
fs::path loose(const Store &store, const ObjectKey &key) {
  return store.root() / "objects" / kind_name(key.kind) / key.id.substr(0, 2) / key.id;
}
std::map<fs::path, Bytes> files(const fs::path &root) {
  std::map<fs::path, Bytes> result;
  for (const auto &file : fs::recursive_directory_iterator(root))
    if (file.is_regular_file())
      result.emplace(file.path(), read_bytes(file.path()));
  return result;
}
ObjectKey ingest(const Store &store, std::size_t seed) {
  std::string data(257 * 64 + 7, '\0');
  std::uint64_t state = seed;
  for (auto &c : data) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    c = static_cast<char>(state);
  }
  std::istringstream input(data);
  return {Kind::manifest, store.ingest(input, Chunking::fixed_size(64)).first};
}
ObjectKey revision(const Store &store, const ObjectKey &manifest, std::string_view message) {
  return store
      .put(Kind::revision,
           bytes(Json{{"version", 1},
                      {"parents", Json::array()},
                      {"message", message},
                      {"timestamp", 0},
                      {"files", Json{{"asset", Json{{"manifest", manifest.id}, {"executable", false}}}}}}
                     .dump()))
      .first;
}
// Fault fixture publication deliberately bypasses fsck, like a damaged external
// catalog. Production pack() correctly refuses these invalid dependency graphs.
void pack_unchecked(const Store &store) {
  auto keys = store.objects();
  auto pack = PackStorage::build(store.root(), keys,
                                 [&](const ObjectKey &key) { return read_bytes(loose(store, key)); });
  auto config = parse_json(read_bytes(store.root() / "config.json"));
  config["version"] = 2;
  config["pack"] = pack.metadata();
  atomic_write(store.root() / "config.json", bytes(config.dump()), true);
  for (const auto &key : keys)
    fs::remove(loose(store, key));
}
} // namespace
int main(int argc,char **argv) {
  const auto base = fs::temp_directory_path() / path_from_utf8("syrnck-audit-" + hex(random_bytes(16)));
  struct Cleanup {
    fs::path path;
    ~Cleanup(){std::error_code error;fs::remove_all(path,error);}
  } cleanup{base};
  try {
    fixture_test::TestCases cases(fixture_test::CaseSelection::parse(argc,argv));
    for(bool encrypted:{false,true})for(int layout=0;layout<3;++layout)
      cases.add("layout-"+std::to_string(encrypted)+'-'+std::to_string(layout),[encrypted,layout](const fs::path &root){
          std::optional<Key> secret;
          if (encrypted) {
            secret.emplace();
            secret->fill(53);
          }
          Store store(root / (std::to_string(encrypted) + "-" + std::to_string(layout)), secret, true,
                      {.decoded_cache_bytes = 0, .verified_cache_entries = 0, .pack_index_cache_bytes = 0});
          auto a = ingest(store, 123), b = ingest(store, 456), c = ingest(store, 789);
          auto tip = revision(store, a, "retained");
          revision(store, b, "unreachable revision"); // c is an unreachable manifest.
          store.put(Kind::chunk, bytes("standalone unreachable chunk"));
          store.set_ref("main", tip);
          if (layout) {
            auto duplicate = store.objects().front();
            auto envelope = read_bytes(loose(store, duplicate));
            store.pack({.max_objects_per_pack = 257});
            if (layout == 2) {
              atomic_write(loose(store, duplicate), envelope, false);
              ingest(store, 987);
              store.put(Kind::chunk, bytes("additional loose chunk"));
            }
          }
          auto expected = store.objects().size();
          auto before = files(store.root());
          store.reset_io_stats();
          check(store.fsck() == expected && files(store.root()) == before);
          if (layout != 2) {
            check(store.io_stats().object_reads == expected);
            check(store.io_stats().decoded_objects == expected);
          }
          store.remove_ref("main");
          check(store.fsck() == expected); // No retention roots must still audit every object.
          (void)c;
      });
    for(bool packed:{false,true})for(int fault=0;fault<3;++fault)
      cases.add("fault-"+std::to_string(packed)+'-'+std::to_string(fault),[packed,fault](const fs::path &root){
          Store store(root / ("fault-" + std::to_string(packed) + "-" + std::to_string(fault)), {}, true);
          auto manifest = ingest(store, 71);
          auto j = store.metadata(Kind::manifest, manifest.id);
          if (fault == 0)
            j["file_hash"] = std::string(64, '0');
          else if (fault == 1)
            j["blocks"][0]["id"] = std::string(64, '0');
          if (fault < 2)
            store.put(Kind::manifest, bytes(j.dump()));
          else
            revision(store, {Kind::manifest, std::string(64, '0')}, "missing file");
          if (packed)
            pack_unchecked(store);
          auto before = files(store.root());
          fails([&] { store.fsck(); });
          check(files(store.root()) == before);
      });
    cases.add("inventory",[](const fs::path &root){
      Store store(root / "inventory", {}, true);
      check(store.fsck() == 0);
      auto manifest = ingest(store, 64);
      store.pack();
      atomic_write(store.root() / "refs.json",
                   bytes(Json{{"broken", key_json({Kind::revision, std::string(64, '0')})}}.dump()), true);
      auto before = files(store.root());
      fails([&] { store.fsck(); });
      check(files(store.root()) == before);
      atomic_write(store.root() / "refs.json", bytes("{}"), true);
      auto invalid = store.root() / "objects" / "chunk" / "ff" / "invalid";
      atomic_write(invalid, bytes("x"), false);
      before = files(store.root());
      fails([&] { store.fsck(); });
      check(files(store.root()) == before);
      fs::remove(invalid);
      std::ostringstream output;
      store.reconstruct(manifest.id, output);
      check(store.fsck() == store.objects().size());
    });
    for(bool encrypted:{false,true})
      cases.add("duplicates-"+std::to_string(encrypted),[encrypted](const fs::path &root){
        std::optional<Key> secret;
        if (encrypted) {
          secret.emplace();
          secret->fill(19);
        }
        Store store(root / ("duplicates-" + std::to_string(encrypted)), secret, true);
        auto object = store.put(Kind::chunk, bytes("duplicate object")).first;
        auto envelope = read_bytes(loose(store, object));
        store.pack();
        atomic_write(loose(store, object), envelope, false);
        auto config = parse_json(read_bytes(store.root() / "config.json"));
        auto path = store.root() / "packs" /
                    path_from_utf8("pack-" + config["pack"]["name"].get<std::string>() + ".syp");
        auto original = read_bytes(path);
        auto corrupt = original;
        corrupt.back() ^= 1;
        store.get(object); // Warm decoded cache and page cache first.
        atomic_write(path, corrupt, true);
        auto before = files(store.root());
        fails([&] { store.fsck(); });
        check(files(store.root()) == before);
        store.reset_io_stats();
        auto data = store.get(object);
        check(std::string(data.begin(), data.end()) == "duplicate object" &&
              store.io_stats().cache_data_hits == 1);
        atomic_write(path, original, true);
        auto bad_loose = envelope;
        bad_loose.back() ^= 1;
        atomic_write(loose(store, object), bad_loose, true);
        fails([&] { store.fsck(); });
        atomic_write(loose(store, object), envelope, true);
        check(store.fsck() == 1);
      });
    return cases.run(base);
  } catch(const std::exception &error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}
}
