#include "pack_storage.hpp"
#include <blake3.h>
#include <zstd.h>
#include <algorithm>
#include <chrono>
#include <iostream>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#endif
using namespace syrnck;
namespace {
using Clock = std::chrono::steady_clock;
Json memory() {
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS_EX value{};
  value.cb = sizeof(value);
  require(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&value),
                               sizeof(value)) != 0,
          "cannot read process memory");
  return Json{{"working_set_bytes", value.WorkingSetSize},
              {"peak_working_set_bytes", value.PeakWorkingSetSize},
              {"private_bytes", value.PrivateUsage}};
#else
  return nullptr;
#endif
}
Bytes payload(std::size_t value) {
  Bytes out(64);
  std::uint64_t state = value + 1;
  for (auto &c : out) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    c = static_cast<std::uint8_t>(state);
  }
  return out;
}
void prepare(const fs::path &root, std::size_t count) {
  require(!fs::exists(root) && count >= 4096 && count % 4096 == 0 && count <= 524288,
          "prepare needs a new directory and 4096..524288 chunks, multiple of 4096");
  {
    Store store(root, {}, true);
  }
  // Fixture construction is a separate process; these collections are excluded
  // from GC measurements. Every envelope and metadata object is real and valid.
  std::map<ObjectKey, Bytes> objects;
  std::map<std::string, Json> sorted_files;
  for (std::size_t group = 0; group < count / 2048; ++group) {
    Json blocks = Json::array();
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    for (std::size_t i = 0; i < 2048; ++i) {
      auto data = payload(group * 2048 + i);
      blake3_hasher_update(&hasher, data.data(), data.size());
      ObjectKey key{Kind::chunk, object_id(Kind::chunk, data)};
      blocks.push_back(Json{{"id", key.id}, {"size", data.size()}});
      objects.emplace(std::move(key), std::move(data));
    }
    Key digest{};
    blake3_hasher_finalize(&hasher, digest.data(), digest.size());
    auto serialized = Json{
        {"version", 1},
        {"size", 2048 * 64},
        {"file_hash", hex(digest)},
        {"chunking", Chunking::fixed_size(64).json()},
        {"blocks",
         blocks}}.dump();
    Bytes data(serialized.begin(), serialized.end());
    ObjectKey key{Kind::manifest, object_id(Kind::manifest, data)};
    if (group % 2 == 0)
      sorted_files["file-" + std::to_string(group)] = Json{{"manifest", key.id}, {"executable", false}};
    objects.emplace(std::move(key), std::move(data));
  }
  Json files = Json::object();
  for (const auto &[name, entry] : sorted_files)
    files[name] = entry;
  auto serialized_revision = Json{
      {"version", 1},
      {"parents", Json::array()},
      {"message", "GC benchmark"},
      {"timestamp", 0},
      {"files", files}}.dump();
  Bytes revision(serialized_revision.begin(), serialized_revision.end());
  ObjectKey tip{Kind::revision, object_id(Kind::revision, revision)};
  objects.emplace(tip, std::move(revision));
  auto pack = PackStorage::build_stream(
      root, objects.size(),
      [&](const PackStorage::KeyVisitor &emit) {
        for (const auto &[key, data] : objects) {
          (void)data;
          emit(key);
        }
      },
      [&](const ObjectKey &key) {
        const auto &raw = objects.at(key);
        validate_object(key.kind, raw);
        Bytes encoded(ZSTD_compressBound(raw.size()) + 5);
        std::copy_n("SYR1\0", 5, encoded.begin());
        auto length = ZSTD_compress(encoded.data() + 5, encoded.size() - 5, raw.data(), raw.size(), 3);
        require(!ZSTD_isError(length), "benchmark compression failed");
        encoded.resize(length + 5);
        return encoded;
      });
  atomic_write(root / "config.json",
               bytes(Json{{"version", 2}, {"key_check", nullptr}, {"pack", pack.metadata()}}.dump()), true);
  atomic_write(root / "refs.json", bytes(Json{{"main", key_json(tip)}}.dump()), true);
  auto expected = Json{{"objects", objects.size()},
                       {"live_objects", count / 2 + count / 4096 + 1},
                       {"unreachable_objects", count / 2 + count / 4096},
                       {"chunks", count},
                       {"block_bytes", 64},
                       {"files", count / 2048},
                       {"pack", pack.metadata()}};
  atomic_write(root / "fixture.json", bytes(expected.dump()), false);
  std::cout << expected.dump() << '\n';
}
Json materialized_gc(Store &store) {
  // Comparison implementation of the previous GC's retained collections.
  // Use current codecs/indexes. This is not a historical executable comparison.
  auto config = parse_json(read_bytes(store.root() / "config.json"));
  PackStorage packs(store.root(), 0); // The Store already owns the 4 MiB page cache.
  packs.load(config.at("pack"));
  auto session = packs.read_session();
  std::vector<ObjectKey> roots;
  for (const auto &[name, key] : store.refs()) {
    (void)name;
    roots.push_back(key);
  }
  auto live = store.closure(roots, true);
  std::vector<ObjectKey> dead;
  std::uint64_t size = 0;
  for (const auto &key : store.objects())
    if (!live.contains(key)) {
      dead.push_back(key);
      size += packs.stored_size(key);
    }
  auto groups = packs.groups();
  std::size_t rewritten = 0, dropped = 0;
  std::uint64_t rewrite_bytes = 0;
  for (const auto &group : groups) {
    std::size_t survivors = 0;
    std::uint64_t length = 8;
    for (const auto &key : group)
      if (live.contains(key)) {
        ++survivors;
        length += packs.stored_size(key);
      }
    if (!survivors)
      ++dropped;
    else if (survivors < group.size()) {
      ++rewritten;
      rewrite_bytes += length;
    }
  }
  return Json{{"live_objects", live.size()},        {"unreachable_objects", dead.size()},
              {"reclaimable_bytes", size},          {"packs_to_rewrite", rewritten},
              {"packs_to_drop", dropped},           {"pack_bytes_to_write", rewrite_bytes},
              {"memory_with_inventories", memory()}};
}
Json materialized_audit(Store &store) {
  // Reproduce the old fsck inventories with current readers/codecs. This fixture
  // has no loose duplicates; both paths still enumerate/check their existence.
  auto config = parse_json(read_bytes(store.root() / "config.json"));
  PackStorage packs(store.root(), 0);
  packs.load(config.at("pack"));
  for (const auto &key : packs.keys()) {
    auto path = store.root() / "objects" / kind_name(key.kind) / key.id.substr(0, 2) / key.id;
    require(!fs::exists(path), "audit benchmark requires a packed-only fixture");
  }
  auto all = store.objects();
  auto roots = all;
  for (const auto &[name, key] : store.refs()) {
    (void)name;
    roots.push_back(key);
  }
  auto verified = store.closure(roots, true);
  require(verified.size() == all.size(), "audit closure differs from inventory");
  return Json{{"objects", all.size()}, {"memory_with_inventories", memory()}};
}
} // namespace
int main(int argc, char **argv) {
  try {
    require(argc >= 3, "usage: gc_bench prepare DIRECTORY CHUNKS | "
                       "materialized|stream|delete|audit-materialized|audit-stream DIRECTORY");
    auto root = path_from_utf8(argv[2]);
    std::string mode = argv[1];
    if (mode == "prepare") {
      require(argc == 4, "missing chunk count");
      prepare(root, static_cast<std::size_t>(std::stoull(argv[3])));
      return 0;
    }
    const bool audit = mode == "audit-materialized" || mode == "audit-stream";
    require(argc == 3 && (mode == "materialized" || mode == "stream" || mode == "delete" || audit),
            "invalid mode");
    auto expected = parse_json(read_bytes(root / "fixture.json"));
    Store store(root, {}, false, {.decoded_cache_bytes = 0, .verified_cache_entries = 0});
    auto before = memory();
    auto start = Clock::now();
    if (audit) {
      auto result =
          mode == "audit-materialized" ? materialized_audit(store) : Json{{"objects", store.fsck()}};
      auto seconds = std::chrono::duration<double>(Clock::now() - start).count();
      auto after = memory();
      auto io = store.io_stats();
      require(result["objects"] == expected["objects"] &&
                  io.object_reads == expected["objects"].get<std::size_t>() &&
                  io.decoded_objects == io.object_reads,
              "audit count or single-pass object reads differ");
      std::cout << Json{{"mode", mode},
                        {"seconds", seconds},
                        {"memory_before", before},
                        {"memory_after", after},
                        {"result", result},
                        {"io", io.json()},
                        {"all_file_hashes_verified", true}}
                       .dump()
                << '\n';
      return 0;
    }
    auto result = mode == "materialized" ? materialized_gc(store) : store.gc(mode == "delete");
    auto seconds = std::chrono::duration<double>(Clock::now() - start).count();
    auto after = memory();
    require(result["live_objects"] == expected["live_objects"] &&
                result["unreachable_objects"] == expected["unreachable_objects"],
            "GC reachability differs");
    if (mode == "delete")
      require(store.fsck() == expected["live_objects"].get<std::size_t>() &&
                  store.gc()["unreachable_objects"] == 0,
              "post-GC reconstruction audit failed");
    std::cout << Json{{"mode", mode},
                      {"seconds", seconds},
                      {"memory_before", before},
                      {"memory_after", after},
                      {"result", result},
                      {"io", store.io_stats().json()},
                      {"all_live_files_verified", true},
                      {"post_delete_audit", mode == "delete"}}
                     .dump()
              << '\n';
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
