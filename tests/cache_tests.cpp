#include <syrnck/core.hpp>
#include <algorithm>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
using namespace syrnck;

namespace {
void check(bool value, std::string_view message = "cache test failed") {
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
auto view(const std::string &value) {
  return std::span(reinterpret_cast<const std::uint8_t *>(value.data()), value.size());
}
fs::path location(const Store &store, const ObjectKey &key) {
  return store.root() / "objects" / kind_name(key.kind) / key.id.substr(0, 2) / key.id;
}
Bytes read(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  return Bytes(std::istreambuf_iterator<char>(in), {});
}
void write(const fs::path &path, const Bytes &bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  check(out.good());
}
std::string sample(std::size_t size) {
  std::string result(size, '\0');
  std::uint64_t state = 0x123456789abcdef0ULL;
  for (auto &c : result) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    c = static_cast<char>(state);
  }
  return result;
}
} // namespace

int main() {
  auto root = fs::temp_directory_path() / path_from_utf8("syrnck-cache-" + hex(random_bytes(16)));
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
    test("hits still read disk, avoid codecs and return independent copies", [&] {
      for (bool encrypted : {false, true}) {
        std::optional<Key> key;
        if (encrypted) {
          key.emplace();
          key->fill(23);
        }
        Store store(root / (encrypted ? "hits-encrypted" : "hits-plain"), key, true);
        auto data = sample(128 * 1024);
        auto object = store.put(Kind::chunk, view(data)).first;
        auto first = store.get(object);
        first[0] ^= 1;
        check(store.contains(object));
        auto second = store.get(object);
        check(std::equal(second.begin(), second.end(), view(data).begin(), view(data).end()));
        auto stats = store.io_stats();
        check(stats.object_reads == 3 && stats.decoded_objects == 1);
        check(stats.cache_presence_hits == 1 && stats.cache_data_hits == 1);
        check(stats.envelope_bytes_read == fs::file_size(location(store, object)) * 3);
        check(stats.cached_bytes == data.size() && stats.cached_payloads == 1);
        store.reset_io_stats();
        check(store.io_stats().object_reads == 0 && store.io_stats().cached_bytes == data.size());
        store.clear_cache();
        check(store.io_stats().cached_objects == 0);
        check(store.get(object) == second && store.io_stats().decoded_objects == 1);
      }
    });
    test("same-size corruption with preserved mtime is rejected even after warming", [&] {
      for (bool encrypted : {false, true}) {
        std::optional<Key> key;
        if (encrypted) {
          key.emplace();
          key->fill(41);
        }
        Store store(root / (encrypted ? "tamper-encrypted" : "tamper-plain"), key, true);
        auto object = store.put(Kind::chunk, view(std::string("retained object"))).first;
        auto path = location(store, object);
        auto original = read(path);
        auto stamp = fs::last_write_time(path);
        (void)store.get(object);
        auto changed = original;
        changed.back() ^= 1;
        write(path, changed);
        fs::last_write_time(path, stamp);
        fails([&] { (void)store.contains(object); });
        fails([&] { (void)store.get(object); });
        check(store.io_stats().cached_objects == 0);
        write(path, original);
        check(store.contains(object));
        fs::remove(path);
        check(!store.contains(object) && store.io_stats().cached_objects == 0);
        fails([&] { (void)store.get(object); });
        write(path, original);
        check(store.contains(object));
      }
    });
    test("valid rewritten encrypted envelopes are revalidated and replace cached evidence", [&] {
      Key key{};
      key.fill(13);
      Store store(root / "rewritten", key, true), other(root / "rewritten-other", key, true);
      auto data = sample(8192);
      auto object = store.put(Kind::chunk, view(data)).first;
      check(other.put(Kind::chunk, view(data)).first == object);
      auto expected = store.get(object);
      auto replacement = read(location(other, object));
      check(replacement != read(location(store, object)));
      auto stamp = fs::last_write_time(location(store, object));
      write(location(store, object), replacement);
      fs::last_write_time(location(store, object), stamp);
      store.reset_io_stats();
      check(store.get(object) == expected);
      check(store.io_stats().decoded_objects == 1 && store.io_stats().cache_data_hits == 0);
      check(store.get(object) == expected && store.io_stats().cache_data_hits == 1);
    });
    test("payload and evidence LRU have independent bounds", [&] {
      Store store(root / "lru", {}, true, StoreOptions{8, 3});
      std::vector<ObjectKey> keys;
      for (auto data : {"aaaaaaaa", "bbbbbbbb", "cccccccc", "dddddddd"})
        keys.push_back(store.put(Kind::chunk, view(std::string(data))).first);
      (void)store.get(keys[0]);
      (void)store.get(keys[1]);
      store.reset_io_stats();
      check(store.contains(keys[0])); // Payload evicted, evidence still present.
      check(store.io_stats().cache_presence_hits == 1 && store.io_stats().decoded_objects == 0);
      (void)store.get(keys[2]);
      (void)store.get(keys[3]); // Evidence for key 1 is least recently used.
      check(store.io_stats().cached_objects == 3 && store.io_stats().cached_payloads == 1);
      check(store.io_stats().cached_bytes == 8);
      store.reset_io_stats();
      check(store.contains(keys[1]));
      check(store.io_stats().decoded_objects == 1 && store.io_stats().cache_presence_hits == 0);
      check(store.io_stats().cached_objects <= 3 && store.io_stats().cached_bytes <= 8);
    });
    test("evidence-only and fully disabled cache modes work", [&] {
      for (bool enabled : {false, true}) {
        Store store(root / (enabled ? "evidence" : "disabled"), {}, true, StoreOptions{0, enabled ? 4U : 0U});
        auto object = store.put(Kind::chunk, view(std::string("payload"))).first;
        (void)store.get(object);
        check(store.contains(object));
        (void)store.get(object);
        auto stats = store.io_stats();
        check(stats.cached_bytes == 0 && stats.cached_payloads == 0 && stats.cache_data_hits == 0);
        check(stats.decoded_objects == (enabled ? 2 : 3));
        check(stats.cache_presence_hits == (enabled ? 1 : 0));
      }
    });
    test("complete closures stream each unique file block once with cache disabled", [&] {
      Store store(root / "single-pass", {}, true, StoreOptions{0, 0});
      std::istringstream input(sample(3 * 65536 + 7));
      auto [id, stats] = store.ingest(input, Chunking::fixed_size(65536));
      auto expected = stats.blocks + 1;
      store.reset_io_stats();
      check(store.closure({{Kind::manifest, id}}, true).size() == expected);
      check(store.io_stats().object_reads == expected && store.io_stats().decoded_objects == expected);
      store.reset_io_stats();
      store.set_ref("release", {Kind::manifest, id});
      check(store.io_stats().object_reads == expected);
      store.reset_io_stats();
      check(store.fsck() == expected && store.io_stats().decoded_objects == expected);
    });
    test("fsck and GC bypass warmed cache, then restore normal reads", [&] {
      Store store(root / "audit", {}, true);
      std::istringstream input(sample(3 * 65536 + 7));
      auto id = store.ingest(input, Chunking::fixed_size(65536)).first;
      ObjectKey object{Kind::manifest, id};
      store.set_ref("release", object);
      auto expected = store.objects().size();
      store.reset_io_stats();
      check(store.fsck() == expected);
      check(store.io_stats().decoded_objects == expected && store.io_stats().cache_data_hits == 0);
      store.reset_io_stats();
      check(store.gc().at("live_objects").get<std::size_t>() == expected);
      check(store.io_stats().decoded_objects == expected && store.io_stats().cache_data_hits == 0);
      (void)store.get(object);
      check(store.io_stats().cache_data_hits == 1);
    });
    test("failed audit restores cache mode and GC never resurrects deleted objects", [&] {
      Store store(root / "audit-failure", {}, true);
      auto live = store.put(Kind::chunk, view(std::string("live"))).first;
      auto dead = store.put(Kind::chunk, view(std::string("dead"))).first;
      store.set_ref("release", live);
      (void)store.get(dead);
      auto path = location(store, live);
      auto original = read(path), changed = original;
      changed[0] ^= 1;
      write(path, changed);
      fails([&] { store.gc(true); });
      check(fs::exists(location(store, dead)), "GC deleted before validating live data");
      write(path, original);
      store.reset_io_stats();
      (void)store.get(dead);
      check(store.io_stats().cache_data_hits == 1, "audit left cache bypass active");
      store.gc(true);
      check(!store.contains(dead));
      fails([&] { (void)store.get(dead); });
      check(store.contains(live));
    });
    test("a cached manifest still requires its full file hash before publication", [&] {
      Store store(root / "bad-file-hash", {}, true);
      std::istringstream input("original bytes");
      auto id = store.ingest(input).first;
      store.set_ref("release", {Kind::manifest, id});
      auto before = store.refs();
      auto manifest = store.metadata(Kind::manifest, id);
      manifest["file_hash"] = std::string(64, '0');
      auto bad = store.put(Kind::manifest, view(manifest.dump())).first;
      (void)store.get(bad);
      fails([&] { store.set_ref("release", bad); });
      fails([&] { store.restore_file(bad.id, root / "bad-output"); });
      check(store.refs() == before && !fs::exists(root / "bad-output"));
    });
    test("cache is process-local and invalid limits fail before creating a store", [&] {
      ObjectKey object;
      {
        Store store(root / "reopened", {}, true);
        object = store.put(Kind::chunk, view(std::string("persistent object"))).first;
        (void)store.get(object);
      }
      Store reopened(root / "reopened");
      check(reopened.io_stats().cached_objects == 0);
      (void)reopened.get(object);
      check(reopened.io_stats().decoded_objects == 1);
      for (auto options : {StoreOptions{1024ULL * 1024 * 1024 + 1, 1}, StoreOptions{1, 1024 * 1024 + 1}})
        fails([&] { Store invalid(root / "invalid", {}, true, options); });
      check(!fs::exists(root / "invalid"));
    });
    std::cout << count << " verified-cache checks passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
}
