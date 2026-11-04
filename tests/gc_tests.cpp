#include "object_marks.hpp"
#include "pack_index_fixture.hpp"
#include <iostream>
#include <sstream>
using namespace syrnck;
namespace fixture = index_fixture;
namespace {
void check(bool value, std::string_view message = "streaming GC test failed") { require(value, message); }
template <class Function> void fails(Function function) {
  bool failed = false;
  try {
    function();
  } catch (const std::exception &) {
    failed = true;
  }
  check(failed, "expected failure");
}
fs::path loose(const Store &store, const ObjectKey &key) {
  return store.root() / "objects" / kind_name(key.kind) / key.id.substr(0, 2) / key.id;
}
Json catalog(const Store &store) {
  auto j = parse_json(read_bytes(store.root() / "config.json"));
  if (j["version"] == 3)
    return j["packs"];
  return j.contains("pack") && !j["pack"].is_null() ? Json::array({j["pack"]}) : Json::array();
}
std::map<fs::path, Bytes> files(const fs::path &root) {
  std::map<fs::path, Bytes> result;
  for (const auto &file : fs::recursive_directory_iterator(root))
    if (file.is_regular_file())
      result.emplace(file.path(), read_bytes(file.path()));
  return result;
}
} // namespace
int main() {
  auto root = fs::temp_directory_path() / path_from_utf8("syrnck-gc-" + hex(random_bytes(16)));
  fs::create_directory(root);
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code ec;
      fs::remove_all(path, ec);
    }
  } cleanup{root};
  std::size_t count = 0;
  auto test = [&](std::string_view name, auto function) {
    function();
    ++count;
    std::cout << "PASS " << name << '\n';
  };
  try {
    test("bitmap positions cross byte/page/pack boundaries and preserve typed keys", [&] {
      auto directory = root / "bitmap";
      auto a = fixture::create(directory, 513);
      auto b = fixture::create(directory, 257, 0, 2, Kind::manifest);
      PackStorage packs(directory, 0);
      packs.load(Json::array({a, b}));
      ObjectMarks marks(packs);
      check(marks.packed_bytes() == 65 + 33);
      for (std::size_t segment = 0; segment < 2; ++segment)
        packs.visit(segment, [&](const ObjectKey &key, PackStorage::Position position) {
          check(position.segment == segment && position.length == 5);
          auto located = packs.locate(key);
          check(located && located->ordinal == position.ordinal && located->segment == segment);
          // Reentrant reads change the zero-cache scratch, including another pack/page.
          auto other = fixture::key(512, segment == 0 ? Kind::manifest : Kind::chunk).object();
          check(packs.read(other).size() == 5);
          if (position.ordinal % 3 == 0) {
            check(marks.insert(key) && !marks.insert(key));
          }
        });
      for (std::size_t segment = 0; segment < 2; ++segment)
        packs.visit(segment, [&](const ObjectKey &key, PackStorage::Position position) {
          check(marks.contains(position) == (position.ordinal % 3 == 0));
          check(marks.contains(key) == marks.contains(position));
        });
      auto absent = fixture::key(1).object();
      check(marks.insert(absent) && !marks.insert(absent) && marks.loose_contains(absent));
      check(marks.size() == 171 + 86 + 1 && marks.loose_size() == 1);
      check(packs.open_streams() == 0 && packs.open_index_streams() == 0);
    });
    test("streamed writer rejects count/order/source failures without changing published sources", [&] {
      Store store(root / "writer", {}, true);
      store.put(Kind::chunk, bytes("a"));
      store.put(Kind::chunk, bytes("b"));
      auto keys = store.objects();
      auto before = files(store.root());
      auto read = [&](const ObjectKey &key) { return read_bytes(loose(store, key)); };
      for (std::size_t advertised : {0, 1, 3})
        fails([&] {
          PackStorage::build_stream(
              store.root(), advertised,
              [&](const PackStorage::KeyVisitor &emit) {
                for (const auto &key : keys)
                  emit(key);
              },
              read);
        });
      fails([&] {
        PackStorage::build_stream(
            store.root(), 2,
            [&](const PackStorage::KeyVisitor &emit) {
              emit(keys[1]);
              emit(keys[0]);
            },
            read);
      });
      fails([&] {
        PackStorage::build_stream(
            store.root(), 2,
            [&](const PackStorage::KeyVisitor &emit) {
              emit(keys[0]);
              throw Error("source failure");
            },
            read);
      });
      check(files(store.root()) == before);
      auto part = PackStorage::build_stream(
          store.root(), 2,
          [&](const PackStorage::KeyVisitor &emit) {
            for (const auto &key : keys)
              emit(key);
          },
          read);
      check(part.keys() == keys);
      for (const auto &key : keys)
        check(part.read(key) == read(key));
      check(parse_json(read_bytes(store.root() / "config.json"))["version"] == 1);
    });
    test("mixed encrypted/plain GC matches set oracle with duplicates and shifted catalog positions", [&] {
      for (bool encrypted : {false, true}) {
        std::optional<Key> secret;
        if (encrypted) {
          secret.emplace();
          secret->fill(67);
        }
        Store store(root / (encrypted ? "encrypted" : "plain"), secret, true,
                    {.decoded_cache_bytes = 0, .verified_cache_entries = 0, .pack_index_cache_bytes = 0});
        std::vector<ObjectKey> keys;
        std::map<ObjectKey, Bytes> duplicates;
        for (std::size_t i = 0; i < 600; ++i) {
          auto payload = "block-" + std::to_string(i);
          payload.resize(16, '_');
          auto key = store.put(Kind::chunk, bytes(payload)).first;
          keys.push_back(key);
          if (i % 7 == 0)
            duplicates.emplace(key, read_bytes(loose(store, key)));
        }
        store.pack({.max_objects_per_pack = 257});
        // Add another pack so deleting/rewriting earlier packs changes positions.
        auto extra = store.put(Kind::chunk, bytes("extra retained pack")).first;
        store.set_ref("extra", extra);
        store.pack();
        for (const auto &[key, envelope] : duplicates)
          atomic_write(loose(store, key), envelope, false);
        auto new_live = store.put(Kind::chunk, bytes("loose retained")).first;
        store.put(Kind::chunk, bytes("loose dead"));
        store.set_ref("loose", new_live);
        std::string data;
        for (std::size_t i = 0; i < keys.size(); i += 4) {
          auto block = store.get(keys[i]);
          data.append(block.begin(), block.end());
        }
        // Repeat a shared chunk to validate file hashing independently of marking.
        auto shared = store.get(keys[0]);
        data.append(shared.begin(), shared.end());
        std::istringstream input(data);
        auto manifest = store.ingest(input, Chunking::fixed_size(16)).first;
        store.set_ref("file", {Kind::manifest, manifest});
        std::vector<ObjectKey> roots;
        for (const auto &[name, key] : store.refs()) {
          (void)name;
          roots.push_back(key);
        }
        auto live = store.closure(roots, true);
        auto all = store.objects();
        PackStorage packs(store.root(), 0);
        packs.load(catalog(store));
        std::size_t dead = 0;
        std::uint64_t dead_bytes = 0;
        for (const auto &key : all)
          if (!live.contains(key)) {
            ++dead;
            if (fs::exists(loose(store, key)))
              dead_bytes += fs::file_size(loose(store, key));
            if (packs.contains(key))
              dead_bytes += packs.stored_size(key);
          }
        auto before = files(store.root());
        auto dry = store.gc();
        check(files(store.root()) == before);
        check(dry["unreachable_objects"] == dead && dry["live_objects"] == live.size());
        check(dry["reclaimable_bytes"] == dead_bytes && dry["packed_mark_bytes"] == 33 + 33 + 11 + 1);
        auto removed = store.gc(true);
        dry["deleted"] = true;
        check(dry == removed);
        auto retained = store.objects();
        check(std::set<ObjectKey>(retained.begin(), retained.end()) == live);
        for (const auto &[key, envelope] : duplicates) {
          (void)envelope;
          check(fs::exists(loose(store, key)) == live.contains(key));
        }
        std::ostringstream output;
        store.reconstruct(manifest, output);
        check(output.str() == data && store.fsck() == live.size());
        for (const auto &[name, key] : store.refs()) {
          (void)key;
          store.remove_ref(name);
        }
        check(store.gc(true)["unreachable_objects"] == live.size() && store.objects().empty());
      }
    });
    test("loose-only marks and empty stores require no packed bitmap", [&] {
      Store store(root / "loose", {}, true);
      check(store.gc(true)["packed_mark_bytes"] == 0);
      auto live = store.put(Kind::chunk, bytes("live")).first;
      for (std::size_t i = 0; i < 96; ++i)
        store.put(Kind::chunk, bytes("loose garbage " + std::to_string(i)));
      store.set_ref("keep", live);
      auto result = store.gc(true);
      check(result["live_objects"] == 1 && result["loose_live_objects"] == 1);
      check(result["unreachable_objects"] == 96 && result["packed_mark_bytes"] == 0);
      check(store.objects() == std::vector<ObjectKey>{live});
    });
    test("invalid loose inventory aborts before creating replacements or deleting garbage", [&] {
      Store store(root / "invalid-inventory", {}, true);
      auto live = store.put(Kind::chunk, bytes("retained")).first;
      store.put(Kind::chunk, bytes("garbage"));
      store.set_ref("keep", live);
      store.pack();
      fs::create_directories(store.root() / "objects" / "chunk" / "ff");
      atomic_write(store.root() / "objects" / "chunk" / "ff" / "invalid", bytes("x"), false);
      auto before = files(store.root());
      fails([&] { store.gc(true); });
      check(files(store.root()) == before);
    });
    std::cout << count << " streaming GC scenarios passed\n";
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
