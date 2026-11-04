#include "pack_index_fixture.hpp"
#include <fstream>
#include <iostream>
#include <sstream>
using namespace syrnck;
namespace fixture = index_fixture;
namespace {
void check(bool value, std::string_view message = "paged index test failed") {
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
void flip(const fs::path &path, std::size_t offset) {
  std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
  file.seekg(static_cast<std::streamoff>(offset));
  char value{};
  file.read(&value, 1);
  check(file.good());
  value ^= 1;
  file.seekp(static_cast<std::streamoff>(offset));
  file.write(&value, 1);
  file.flush();
  check(file.good());
}
Json config(const fs::path &root) {
  return parse_json(read_bytes(root / "config.json"));
}
} // namespace
int main() {
  auto root = fs::temp_directory_path() / path_from_utf8("syrnck-index-" + hex(random_bytes(16)));
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
    test("typed enumeration skips unrelated pages and preserves physical positions", [&] {
      for (std::size_t chunks : {0, 1, 255, 256, 257, 4097}) {
        auto directory = root / ("typed-" + std::to_string(chunks));
        std::vector<ObjectKey> keys;
        for (std::size_t i = 0; i < chunks; ++i)
          keys.push_back(fixture::key(i, Kind::chunk).object());
        // With no manifests, one boundary page can straddle the missing kind.
        auto manifests = chunks % 2 == 0 ? 0U : 257U;
        for (std::size_t i = 0; i < manifests; ++i)
          keys.push_back(fixture::key(i, Kind::manifest).object());
        for (std::size_t i = 0; i < 255; ++i)
          keys.push_back(fixture::key(i, Kind::revision).object());
        auto built = PackStorage::build(directory, keys,
                                        [](const ObjectKey &) { return Bytes{'S', 'Y', 'R', '1', 'x'}; });
        for (std::size_t budget : {std::size_t{0}, PackIndex::page_bytes}) {
          PackStorage packs(directory, budget);
          packs.load(built.metadata());
          for (auto kind : {Kind::revision, Kind::manifest, Kind::chunk}) {
            std::vector<ObjectKey> selected;
            auto before = packs.index_stats().page_reads;
            packs.visit_kind(0, kind, [&](const ObjectKey &key, PackStorage::Position position) {
              check(position.segment == 0 && position.length == 5 && keys.at(position.ordinal) == key);
              selected.push_back(key);
            });
            std::vector<ObjectKey> expected;
            std::copy_if(keys.begin(), keys.end(), std::back_inserter(expected),
                         [&](const ObjectKey &key) { return key.kind == kind; });
            check(selected == expected);
            std::size_t pages = 0;
            for (std::size_t i = 0; i < keys.size(); i += PackIndex::records_per_page)
              if (keys[i].kind <= kind &&
                  keys[std::min(i + PackIndex::records_per_page, keys.size()) - 1].kind >= kind)
                ++pages;
            check(packs.index_stats().page_reads - before == pages);
            // Nested zero-cache lookups may replace scratch or evict another page.
            packs.visit_kind(0, kind, [&](const ObjectKey &key, PackStorage::Position position) {
              check(packs.read(keys.front()).size() == 5 && packs.read(keys.back()).size() == 5);
              check(keys.at(position.ordinal) == key && packs.locate(key)->ordinal == position.ordinal);
            });
          }
          fails([&] { packs.visit_kind(0, static_cast<Kind>(3), [](const auto &, auto) {}); });
          fails([&] { packs.visit_kind(1, Kind::chunk, [](const auto &, auto) {}); });
          fails(
              [&] { packs.visit_kind(0, Kind::revision, [](const auto &, auto) { throw Error("stop"); }); });
          check(packs.open_streams() == 0 && packs.open_index_streams() == 0);
        }
      }
    });
    test("large index retains only fences/filter and a bounded page cache", [&] {
      auto directory = root / "large";
      constexpr std::size_t records = 262144;
      auto descriptor = fixture::create(directory, records);
      PackStorage packs(directory, 2 * PackIndex::page_bytes);
      packs.load(descriptor);
      auto stats = packs.index_stats();
      check(stats.directory_bytes >= (records / 256) * 98 && stats.directory_bytes < records);
      check(stats.filter_bytes >= records * 10 / 8 && stats.filter_bytes < records * 2 &&
            stats.cache_bytes == 0);
      for (std::size_t i = 0; i < records; ++i)
        check(packs.contains(fixture::key(2 * i).object()), "filter lost a present key");
      stats = packs.index_stats();
      check(stats.page_reads == records / 256 && stats.cache_bytes == 2 * PackIndex::page_bytes);
      check(stats.cache_hits > records / 2 && stats.cached_pages == 2);
      auto before = stats.page_reads;
      for (std::size_t i = 0; i < 10000; ++i)
        check(!packs.contains(fixture::key(2 * ((i * 7919) % records) + 1).object()));
      check(packs.index_stats().page_reads - before < 1000, "negative filter caused too many page reads");
      check(packs.index_stats().cache_bytes <= 2 * PackIndex::page_bytes);
    });
    test("empty, partial and exact page boundaries preserve lookup and enumeration", [&] {
      for (std::size_t records : {1, 255, 256, 257, 511, 512, 513}) {
        auto directory = root / ("boundary-" + std::to_string(records));
        auto descriptor = fixture::create(directory, records);
        for (std::size_t budget : {std::size_t(0), std::size_t(44), PackIndex::page_bytes}) {
          PackStorage packs(directory, budget);
          packs.load(descriptor);
          auto keys = packs.keys();
          check(keys.size() == records && packs.groups().size() == 1);
          for (std::size_t i = 0; i < records; ++i) {
            check(keys[i] == fixture::key(2 * i).object());
            check(packs.stored_size(keys[i]) == 5);
          }
          check(!packs.contains(fixture::key(2 * records).object()));
          check(packs.index_stats().cache_bytes <= budget);
        }
      }
      PackStorage empty(root);
      empty.load(nullptr);
      check(empty.keys().empty() && !empty.contains(fixture::key(0).object()));
      auto descriptor = fixture::create(root / "alias", 1);
      PackStorage alias(root / "alias");
      alias.load(descriptor);
      alias.load(alias.metadata());
      check(alias.contains(fixture::key(0).object()));
    });
    test("cache zero, tail-page admission, eviction and explicit clearing", [&] {
      auto directory = root / "budgets";
      auto descriptor = fixture::create(directory, 513);
      PackStorage packs(directory, 45);
      packs.load(descriptor);
      check(packs.contains(fixture::key(1024).object()) && packs.index_stats().cache_bytes == 45);
      check(packs.contains(fixture::key(0).object()) && packs.index_stats().cache_bytes == 45);
      packs.clear_index_cache();
      check(packs.index_stats().cache_bytes == 0);
      packs.set_index_cache_limit(PackIndex::page_bytes);
      check(packs.contains(fixture::key(0).object()));
      check(packs.contains(fixture::key(512).object()));
      auto reads = packs.index_stats().page_reads;
      check(packs.contains(fixture::key(0).object()) && packs.index_stats().page_reads == reads + 1);
      packs.set_index_cache_limit(0);
      reads = packs.index_stats().page_reads;
      check(packs.contains(fixture::key(0).object()) && packs.contains(fixture::key(0).object()));
      check(packs.index_stats().page_reads == reads + 2 && packs.index_stats().cache_bytes == 0);
    });
    test("changed uncached or evicted pages fail their validated snapshot digest", [&] {
      auto directory = root / "tamper";
      auto descriptor = fixture::create(directory, 1025);
      PackStorage packs(directory, PackIndex::page_bytes);
      packs.load(descriptor);
      check(packs.contains(fixture::key(0).object()));
      auto file = fixture::path(directory, descriptor);
      auto timestamp = fs::last_write_time(file);
      flip(file, 16 + PackIndex::page_bytes + 12);
      fs::last_write_time(file, timestamp);
      fails([&] { packs.contains(fixture::key(600).object()); });
      // A hit in a previously verified page is an immutable snapshot, as the
      // former all-RAM index was. Reloading/auditing must still reject disk damage.
      check(packs.contains(fixture::key(10).object()));
      flip(file, 16 + 12);
      check(packs.contains(fixture::key(12).object()));
      packs.clear_index_cache();
      fails([&] { packs.contains(fixture::key(12).object()); });
      fails([&] { packs.load(descriptor); });
      fails([&] { packs.contains(fixture::key(10).object()); });
    });
    test("missing or resized index pages are errors rather than absence", [&] {
      auto directory = root / "missing";
      auto descriptor = fixture::create(directory, 513);
      auto file = fixture::path(directory, descriptor);
      auto original = read_bytes(file);
      for (bool missing : {false, true}) {
        PackStorage packs(directory, 0);
        packs.load(descriptor);
        if (missing)
          fs::remove(file);
        else
          fs::resize_file(file, original.size() - 1);
        fails([&] { packs.contains(fixture::key(1024).object()); });
        atomic_write(file, original, fs::exists(file));
      }
    });
    test("ranges and sorting are checked across pages even with a recomputed full hash", [&] {
      auto directory = root / "malformed";
      auto descriptor = fixture::create(directory, 513);
      auto file = fixture::path(directory, descriptor);
      auto original = read_bytes(file);
      for (int mode = 0; mode < 3; ++mode) {
        auto changed = original;
        auto offset = 16 + PackIndex::page_bytes;
        if (mode == 0)
          changed[offset + 33] ^= 1;
        if (mode == 1)
          std::copy_n(changed.begin() + offset - 45, 33, changed.begin() + offset);
        if (mode == 2)
          changed[offset] = 3;
        atomic_write(file, changed, true);
        auto bad = descriptor;
        bad["index_hash"] = hash(changed);
        PackStorage packs(directory);
        fails([&] { packs.load(bad); });
      }
    });
    test("streamed multi-pack merge rejects distant duplicates and respects typed IDs", [&] {
      auto directory = root / "merge";
      auto a = fixture::create(directory, 1025, 0, 4);
      auto b = fixture::create(directory, 1025, 2, 4);
      auto duplicate = fixture::create(directory, 1, 4096, 1);
      PackStorage packs(directory, PackIndex::page_bytes);
      packs.load(Json::array({a, b}));
      check(packs.keys().size() == 2050);
      fails([&] { packs.load(Json::array({a, b, duplicate})); });
      auto other_kind = fixture::create(directory, 1025, 0, 4, Kind::manifest);
      packs.load(Json::array({a, b, other_kind}));
      check(packs.keys().size() == 3075);
      check(packs.contains(fixture::key(4096, Kind::manifest).object()));
    });
    test("index and data handles have separate bounded LRUs and close on exceptions", [&] {
      auto directory = root / "index-handles";
      Json catalog = Json::array();
      for (std::size_t i = 0; i < 12; ++i)
        catalog.push_back(fixture::create(directory, 2, i * 8));
      PackStorage packs(directory, 0);
      packs.load(catalog);
      {
        auto session = packs.read_session();
        for (int repeat = 0; repeat < 2; ++repeat)
          for (std::size_t i = 0; i < 12; ++i) {
            check(packs.read(fixture::key(i * 8).object()).size() == 5);
            check(packs.open_index_streams() <= 8 && packs.open_streams() <= 8);
          }
        check(packs.open_index_streams() == 8 && packs.open_streams() == 8);
      }
      check(packs.open_index_streams() == 0 && packs.open_streams() == 0);
      fails([&] {
        auto session = packs.read_session();
        packs.contains(fixture::key(0).object());
        throw Error("stop");
      });
      check(packs.open_index_streams() == 0);
    });
    test("streaming construction preserves exact legacy index bytes across page boundaries", [&] {
      for (std::size_t records : {512, 513}) {
        auto directory = root / ("writer-" + std::to_string(records));
        auto descriptor = fixture::create(directory, records);
        PackStorage source(directory);
        source.load(descriptor);
        auto output = PackStorage::build(directory, source.keys(),
                                         [&](const ObjectKey &key) { return source.read(key); });
        check(read_bytes(fixture::path(directory, descriptor)) ==
              read_bytes(fixture::path(directory, output.metadata())));
        check(output.metadata()["index_hash"] == descriptor["index_hash"]);
      }
    });
    test("real encrypted stores retain format, reconstruct and GC with index cache disabled", [&] {
      Key key{};
      key.fill(75);
      auto directory = root / "real";
      std::string manifest;
      std::string data;
      for (std::size_t i = 0; i < 4096; ++i)
        data += "block" + std::to_string(i) + "\n";
      {
        Store store(directory, key, true, {.pack_index_cache_bytes = 0});
        std::istringstream input(data);
        manifest = store.ingest(input, Chunking::fixed_size(64)).first;
        store.set_ref("keep", {Kind::manifest, manifest});
        store.pack({.max_objects_per_pack = 256});
        auto before = read_bytes(directory / "config.json");
        std::ostringstream out;
        store.reconstruct(manifest, out);
        check(out.str() == data);
        auto stats = store.io_stats();
        check(stats.index_cached_bytes == 0 && stats.index_directory_bytes > 0 &&
              stats.index_filter_bytes > 0);
        check(store.gc(true)["unreachable_objects"] == 0);
        check(read_bytes(directory / "config.json") == before);
      }
      Store store(directory, key, false, {.pack_index_cache_bytes = PackIndex::page_bytes});
      std::ostringstream out;
      store.reconstruct(manifest, out);
      check(out.str() == data);
      check(store.io_stats().index_cached_bytes <= PackIndex::page_bytes);
      store.clear_cache();
      check(store.io_stats().index_cached_bytes == 0);
      check(store.fsck() > 256);
      store.remove_ref("keep");
      store.gc(true);
      check(store.objects().empty());
    });
    test("index cache limit validation precedes filesystem mutation", [&] {
      auto directory = root / "bad-limit";
      fails([&] { Store store(directory, {}, true, {.pack_index_cache_bytes = 1024ULL * 1024 * 1024 + 1}); });
      check(!fs::exists(directory));
      fails([&] { PackStorage packs(directory, 1024ULL * 1024 * 1024 + 1); });
      check(!fs::exists(directory));
    });
    std::cout << count << " paged index scenarios passed\n";
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
