#include "pack_storage.hpp"
#include <algorithm>
#include <iostream>
#include <sstream>
using namespace syrnck;
namespace {
void check(bool value, std::string_view message = "pack catalog test failed") {
  require(value, message);
}
template <class Function> void fails(Function function) {
  bool failed = false;
  try {
    function();
  } catch (const std::exception &) {
    failed = true;
  }
  check(failed, "expected failure");
}
Json config(const fs::path &root) {
  return parse_json(read_bytes(root / "config.json"));
}
Json catalog(const fs::path &root) {
  auto document = config(root);
  if (document["version"] == 3)
    return document["packs"];
  auto result = Json::array();
  if (document.contains("pack") && !document["pack"].is_null())
    result.push_back(document["pack"]);
  return result;
}
fs::path pack_file(const fs::path &root, const Json &metadata, std::string_view extension = ".syp") {
  return root / "packs" /
         path_from_utf8("pack-" + metadata["name"].get<std::string>() + std::string(extension));
}
std::string sample(std::size_t size, std::uint64_t state) {
  std::string result(size, '\0');
  for (auto &c : result) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    c = static_cast<char>(state);
  }
  return result;
}
std::string ingest(Store &store, const std::string &data) {
  std::istringstream source(data);
  auto id = store.ingest(source, Chunking::fixed_size(4096)).first;
  store.set_ref("files/" + id, {Kind::manifest, id});
  return id;
}
void restore(Store &store, const std::string &id, const std::string &data) {
  std::ostringstream output;
  store.reconstruct(id, output);
  check(output.str() == data, "reconstruction differs");
}
std::map<fs::path, Bytes> files(const fs::path &root) {
  std::map<fs::path, Bytes> result;
  for (const auto &file : fs::recursive_directory_iterator(root))
    if (file.is_regular_file())
      result.emplace(file.path(), read_bytes(file.path(), 64 * 1024 * 1024));
  return result;
}
void write_catalog(const fs::path &root, const Json &packs) {
  auto document = config(root);
  auto next = Json{{"version", 3}, {"key_check", document["key_check"]}, {"packs", packs}}.dump();
  atomic_write(root / "config.json", bytes(next), true);
}
} // namespace
int main() {
  auto root = fs::temp_directory_path() / path_from_utf8("syrnck-catalog-" + hex(random_bytes(16)));
  fs::create_directory(root);
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code ec;
      fs::remove_all(path, ec);
    }
  } cleanup{root};
  std::size_t count = 0;
  auto test = [&](std::string_view name, const auto &function) {
    function();
    ++count;
    std::cout << "PASS " << name << '\n';
  };
  try {
    test("segmented plain/encrypted packs preserve bytes and no-op maintenance writes nothing", [&] {
      for (bool encrypted : {false, true}) {
        auto directory = root / (encrypted ? "encrypted" : "plain");
        std::optional<Key> key;
        if (encrypted) {
          key.emplace();
          key->fill(37);
        }
        auto data = sample(5 * 4096, 51);
        std::string id;
        {
          Store store(directory, key, true);
          id = ingest(store, data);
          std::map<ObjectKey, Bytes> envelopes;
          for (const auto &item : store.objects())
            envelopes[item] =
                read_bytes(directory / "objects" / kind_name(item.kind) / item.id.substr(0, 2) / item.id);
          auto result = store.pack({.max_objects_per_pack = 2});
          check(result["new_packs"] == 3 && result["objects_written"] == 6 &&
                config(directory)["version"] == 3);
          auto metadata = catalog(directory);
          PackStorage packs(directory);
          packs.load(metadata);
          for (const auto &part : metadata)
            check(part["objects"] <= 2);
          for (const auto &[item, envelope] : envelopes)
            check(packs.read(item) == envelope);
          restore(store, id, data);
          auto before = files(directory);
          auto again = store.pack();
          check(again["new_packs"] == 0 && again["pack_bytes_written"] == 0 && again["objects_written"] == 0);
          check(files(directory) == before, "no-op maintenance changed stored bytes");
        }
        Store store(directory, key);
        restore(store, id, data);
        check(store.fsck() == 6 && store.gc()["unreachable_objects"] == 0);
      }
    });
    test("incremental maintenance preserves existing packs and explicit compaction merges them", [&] {
      Store store(root / "incremental", {}, true);
      auto data = sample(8192, 70);
      auto id = ingest(store, data);
      store.pack();
      check(config(store.root())["version"] == 2);
      auto previous = catalog(store.root())[0];
      auto path = pack_file(store.root(), previous);
      auto before = read_bytes(path);
      auto timestamp = fs::last_write_time(path);
      auto changed = ingest(store, data + "new data");
      auto appended = store.pack();
      check(appended["pack_count"] == 2 && appended["objects_written"] == 2);
      check(read_bytes(path) == before && fs::last_write_time(path) == timestamp);
      auto compacted = store.pack({.compact = true});
      check(compacted["pack_count"] == 1 && compacted["objects_written"] == 5);
      check(compacted["obsolete_pack_files_removed"] == 4 && !fs::exists(path));
      check(config(store.root())["version"] == 3, "format v3 must not downgrade implicitly");
      restore(store, id, data);
      restore(store, changed, data + "new data");
    });
    test("byte budgets split incompressible envelopes and include the pack header", [&] {
      Store store(root / "byte-budget", {}, true);
      for (std::uint64_t seed : {71, 72}) {
        auto value = sample(10 * 1024 * 1024, seed);
        store.put(Kind::chunk, bytes(value));
      }
      store.put(Kind::chunk, bytes("small"));
      const auto budget = 18ULL * 1024 * 1024;
      auto result = store.pack({.max_pack_bytes = budget});
      check(result["new_packs"] == 2);
      for (const auto &metadata : catalog(store.root())) {
        check(metadata["bytes"].get<std::uint64_t>() <= budget);
        check(fs::file_size(pack_file(store.root(), metadata)) == metadata["bytes"].get<std::uint64_t>());
      }
      check(store.fsck() == 3);
    });
    test("GC rewrites one affected pack and later drops fully dead packs without copying survivors", [&] {
      Store store(root / "selective-gc", {}, true);
      auto shared = sample(4096, 101);
      auto a = shared + sample(4096, 102), b = shared + sample(4096, 103), c = sample(8192, 104);
      auto first = ingest(store, a);
      store.pack();
      auto second = ingest(store, b);
      store.pack();
      auto third = ingest(store, c);
      store.pack();
      auto initial = catalog(store.root());
      auto unaffected = read_bytes(pack_file(store.root(), initial[2]));
      store.remove_ref("files/" + first);
      auto before = files(store.root());
      auto dry = store.gc();
      check(dry["packs_to_rewrite"] == 1 && dry["packs_to_drop"] == 0);
      check(files(store.root()) == before);
      auto result = store.gc(true);
      check(result["unreachable_objects"] == 2 && result["packs_to_rewrite"] == 1);
      auto current = catalog(store.root());
      check(current[0] != initial[0] && current[1] == initial[1] && current[2] == initial[2]);
      restore(store, second, b);
      restore(store, third, c);
      store.remove_ref("files/" + second);
      dry = store.gc();
      check(dry["packs_to_rewrite"] == 0 && dry["packs_to_drop"] == 2 && dry["pack_bytes_to_write"] == 0);
      store.gc(true);
      check(catalog(store.root()).size() == 1 && catalog(store.root())[0] == initial[2]);
      check(read_bytes(pack_file(store.root(), initial[2])) == unaffected);
      restore(store, third, c);
      check(store.fsck() == 3);
    });
    test("read sessions bound open streams across many packs and close after exceptions", [&] {
      Store store(root / "streams", {}, true);
      for (std::size_t i = 1; i <= 12; ++i)
        store.put(Kind::chunk, bytes(std::to_string(i)));
      store.pack({.max_objects_per_pack = 1});
      PackStorage packs(store.root());
      packs.load(catalog(store.root()));
      auto groups = packs.groups();
      {
        auto session = packs.read_session();
        for (int repeat = 0; repeat < 3; ++repeat)
          for (const auto &group : groups) {
            check(!packs.read(group.front()).empty());
            check(packs.open_streams() <= PackStorage::max_open_streams);
          }
        {
          auto nested = packs.read_session();
          (void)packs.read(groups.front().front());
        }
        check(packs.open_streams() == PackStorage::max_open_streams);
      }
      check(packs.open_streams() == 0);
      fails([&] {
        auto session = packs.read_session();
        (void)packs.read(groups[0][0]);
        throw Error("stop");
      });
      check(packs.open_streams() == 0);
      store.pack({.compact = true}); // Windows deletion also proves sessions released their handles.
      check(store.fsck() == 12);
    });
    test("duplicate pack names, duplicate object membership and invalid catalog shapes fail closed", [&] {
      Store store(root / "bad-catalog", {}, true);
      store.put(Kind::chunk, bytes("catalog"));
      store.pack();
      auto original = catalog(store.root());
      auto config_before = read_bytes(store.root() / "config.json");
      auto keys = store.objects();
      PackStorage source(store.root());
      source.load(original);
      auto duplicate =
          PackStorage::build(store.root(), keys, [&](const ObjectKey &key) { return source.read(key); });
      std::vector<Json> invalid{Json::object(), Json::array({original[0], original[0]}),
                                Json::array({original[0], duplicate.metadata()})};
      auto excessive = Json::array();
      for (std::size_t i = 0; i < 5; ++i) {
        auto part = original[0];
        part["name"] = hex(random_bytes(16));
        part["objects"] = PackStorage::max_entries;
        excessive.push_back(part);
      }
      invalid.push_back(excessive);
      auto too_many = Json::array();
      for (std::size_t i = 0; i <= PackStorage::max_packs; ++i)
        too_many.push_back(original[0]);
      invalid.push_back(too_many);
      for (const auto &bad : invalid) {
        write_catalog(store.root(), bad);
        auto before = files(store.root());
        fails([&] { store.fsck(); });
        fails([&] { store.gc(true); });
        fails([&] { store.pack(); });
        check(files(store.root()) == before);
      }
      atomic_write(store.root() / "config.json", config_before, true);
      check(store.fsck() == 1);
    });
    test("unpublished partial batches keep the previous catalog and loose sources recoverable", [&] {
      auto directory = root / "partial-build";
      Store store(directory, {}, true);
      auto first = ingest(store, sample(4096, 200));
      store.pack();
      auto second = ingest(store, sample(8192, 201));
      auto before = read_bytes(directory / "config.json");
      PackStorage active(directory);
      active.load(catalog(directory));
      auto keys = store.objects();
      std::erase_if(keys, [&](const ObjectKey &key) { return active.contains(key); });
      std::size_t reads = 0;
      fails([&] {
        PackStorage::build_many(
            directory, keys,
            [&](const ObjectKey &key) {
              if (++reads == 2)
                throw Error("injected failure in second pack");
              return read_bytes(directory / "objects" / kind_name(key.kind) / key.id.substr(0, 2) / key.id);
            },
            [&](const ObjectKey &key) {
              return fs::file_size(directory / "objects" / kind_name(key.kind) / key.id.substr(0, 2) /
                                   key.id);
            },
            {.max_objects_per_pack = 1});
      });
      check(read_bytes(directory / "config.json") == before && store.gc()["obsolete_pack_files"] == 2);
      restore(store, first, sample(4096, 200));
      restore(store, second, sample(8192, 201));
      auto result = store.pack({.max_objects_per_pack = 1});
      check(result["obsolete_pack_files_removed"] == 2);
      check(store.fsck() == 5);
    });
    test("publication before loose cleanup reopens the complete multi-pack catalog", [&] {
      auto directory = root / "after-publication";
      std::string first, second;
      {
        Store store(directory, {}, true);
        first = ingest(store, sample(4096, 300));
        store.pack();
        second = ingest(store, sample(8192, 301));
        PackStorage active(directory);
        active.load(catalog(directory));
        auto keys = store.objects();
        std::erase_if(keys, [&](const ObjectKey &key) { return active.contains(key); });
        auto next = PackStorage::build_many(
            directory, keys,
            [&](const ObjectKey &key) {
              return read_bytes(directory / "objects" / kind_name(key.kind) / key.id.substr(0, 2) / key.id);
            },
            [&](const ObjectKey &key) {
              return fs::file_size(directory / "objects" / kind_name(key.kind) / key.id.substr(0, 2) /
                                   key.id);
            },
            {.max_objects_per_pack = 1});
        auto combined = active.catalog();
        for (const auto &part : next.catalog())
          combined.push_back(part);
        write_catalog(directory, combined);
      }
      Store store(directory);
      check(store.fsck() == 5);
      auto before = read_bytes(directory / "config.json");
      auto result = store.pack();
      check(result["new_packs"] == 0 && result["loose_objects_removed"] == 3);
      check(read_bytes(directory / "config.json") == before);
      restore(store, first, sample(4096, 300));
      restore(store, second, sample(8192, 301));
    });
    test("corruption in a retained pack forbids deletion of garbage in another pack", [&] {
      Store store(root / "corrupt-live", {}, true);
      auto old = ingest(store, sample(4096, 400));
      store.pack();
      auto live = ingest(store, sample(4096, 401));
      store.pack();
      store.remove_ref("files/" + old);
      restore(store, live, sample(4096, 401)); // Warm decoded cache.
      auto file = pack_file(store.root(), catalog(store.root())[1]);
      auto original = read_bytes(file), bad = original;
      bad[15] ^= 1;
      atomic_write(file, bad, true);
      auto before = files(store.root());
      fails([&] { store.gc(true); });
      fails([&] { store.pack(); });
      check(files(store.root()) == before);
      atomic_write(file, original, true);
      check(store.gc(true)["packs_to_drop"] == 1);
      restore(store, live, sample(4096, 401));
    });
    test("invalid pack options and capacity planning fail before publication or source reads", [&] {
      Store store(root / "limits", {}, true);
      store.put(Kind::chunk, bytes("limits"));
      auto before = files(store.root());
      for (const auto &options : {PackOptions{.max_pack_bytes = 0}, PackOptions{.max_pack_bytes = 1},
                                  PackOptions{.max_pack_bytes = 4ULL * 1024 * 1024 * 1024 + 1},
                                  PackOptions{.max_objects_per_pack = 0},
                                  PackOptions{.max_objects_per_pack = PackStorage::max_entries + 1}})
        fails([&] { store.pack(options); });
      std::vector<ObjectKey> keys{{Kind::chunk, std::string(64, 'a')}, {Kind::chunk, std::string(64, 'b')}};
      bool read = false;
      fails([&] {
        PackStorage::build_many(
            store.root(), keys,
            [&](const ObjectKey &) {
              read = true;
              return Bytes(5);
            },
            [](const ObjectKey &) { return 5; }, {.max_objects_per_pack = 1}, 1);
      });
      check(!read && files(store.root()) == before);
    });
    std::cout << count << " multi-pack scenarios passed\n";
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
