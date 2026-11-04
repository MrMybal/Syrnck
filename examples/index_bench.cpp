#include "pack_index_fixture.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#endif
using namespace syrnck;
namespace fixture = index_fixture;
namespace {
using Clock = std::chrono::steady_clock;
double elapsed(Clock::time_point start) {
  return std::chrono::duration<double>(Clock::now() - start).count();
}
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
std::uint64_t integer(std::span<const std::uint8_t> value) {
  std::uint64_t result = 0;
  for (std::size_t i = 0; i < value.size(); ++i)
    result |= static_cast<std::uint64_t>(value[i]) << (8 * i);
  return result;
}
struct MapEntry {
  std::uint64_t offset;
  std::uint32_t length;
  std::size_t segment;
};
} // namespace
int main(int argc, char **argv) {
  try {
    require(argc >= 4, "usage: index_bench prepare DIRECTORY COUNT | map|paged DIRECTORY QUERIES CACHE_MIB "
                       "random|sequential");
    std::string mode = argv[1];
    auto root = path_from_utf8(argv[2]);
    if (mode == "prepare") {
      require(argc == 4 && !fs::exists(root), "prepare requires a new fixture directory");
      auto count = static_cast<std::size_t>(std::stoull(argv[3]));
      auto descriptor = fixture::create(root, count);
      atomic_write(root / "fixture.json", bytes(descriptor.dump()), false);
      std::cout << descriptor.dump() << '\n';
      return 0;
    }
    require(argc == 6 && (mode == "map" || mode == "paged"), "invalid benchmark arguments");
    auto queries = static_cast<std::size_t>(std::stoull(argv[3]));
    auto cache_mib = static_cast<std::size_t>(std::stoull(argv[4]));
    std::string pattern = argv[5];
    require(queries > 0 && queries <= 1000000 && cache_mib <= 1024 &&
                (pattern == "random" || pattern == "sequential"),
            "invalid query parameters");
    auto descriptor = parse_json(read_bytes(root / "fixture.json"));
    auto count = descriptor["objects"].get<std::size_t>();
    Json before = memory();
    auto start = Clock::now();
    std::unique_ptr<PackStorage> paged;
    std::map<ObjectKey, MapEntry> entries;
    if (mode == "paged") {
      paged = std::make_unique<PackStorage>(root, cache_mib * 1024 * 1024);
      paged->load(descriptor);
    } else {
      // Comparison implementation of the previous full-index + std::map model.
      // Validate the same sorted index, hash and contiguous ranges before lookup.
      auto data = read_bytes(fixture::path(root, descriptor), 64 * 1024 * 1024);
      require(data.size() == 16 + 45 * count && hash(data) == descriptor["index_hash"].get<std::string>(),
              "bad fixture index");
      require(std::equal(data.begin(), data.begin() + 8, "SYRIDX01") &&
                  integer(std::span(data).subspan(8, 8)) == count,
              "bad index header");
      auto pack = fixture::path(root, descriptor, ".syp");
      require(fs::file_size(pack) == descriptor["bytes"].get<std::uint64_t>(), "bad pack size");
      std::ifstream in(pack, std::ios::binary);
      std::array<char, 8> header{};
      in.read(header.data(), 8);
      require(in.good() && std::equal(header.begin(), header.end(), "SYRPACK1"), "bad pack header");
      std::optional<ObjectKey> previous;
      std::uint64_t offset = 8;
      for (std::size_t i = 0; i < count; ++i) {
        auto record = PackIndex::record(std::span(data).subspan(16), i);
        require(record.key.value[0] <= 2 && record.offset == offset && record.length >= 5 &&
                    record.length <= max_object_size + 1024 * 1024,
                "bad index range");
        offset += record.length;
        auto key = record.key.object();
        require(!previous || *previous < key, "unsorted index");
        require(entries.emplace(key, MapEntry{record.offset, record.length, 0}).second, "duplicate key");
        previous = std::move(key);
      }
      require(offset == descriptor["bytes"].get<std::uint64_t>(), "unindexed data");
    }
    auto load_seconds = elapsed(start);
    Json after_load = memory();
    auto pass = [&] {
      auto begin = Clock::now();
      std::size_t hits = 0;
      // Store reconstruction/closure/ingest use the same bounded read session.
      std::unique_ptr<PackStorage::ReadSession> session;
      if (paged)
        session = std::make_unique<PackStorage::ReadSession>(*paged);
      for (std::size_t i = 0; i < queries; ++i) {
        auto ordinal = pattern == "random" ? (i * 7919 + 17) % count : (i / 2) % count;
        bool expected = i % 2 == 0;
        auto key = fixture::key(2 * ordinal + (expected ? 0 : 1)).object();
        bool found = paged ? paged->contains(key) : entries.contains(key);
        require(found == expected, "index lookup differs from fixture");
        hits += found;
      }
      return std::pair(elapsed(begin), hits);
    };
    auto [query_seconds, hits] = pass();
    auto cold_stats = paged ? paged->index_stats() : PackStorage::IndexStats{};
    auto after_first_queries = memory();
    auto [warm_seconds, warm_hits] = pass();
    auto stats = paged ? paged->index_stats() : PackStorage::IndexStats{};
    Json result{{"mode", mode},
                {"pattern", pattern},
                {"objects", count},
                {"queries", queries},
                {"cache_mib", cache_mib},
                {"hits", hits},
                {"all_results_verified", true},
                {"load_seconds", load_seconds},
                {"query_seconds", query_seconds},
                {"queries_per_second", static_cast<double>(queries) / query_seconds},
                {"warm_query_seconds", warm_seconds},
                {"warm_queries_per_second", static_cast<double>(queries) / warm_seconds},
                {"warm_hits", warm_hits},
                {"memory_before", before},
                {"memory_after_load", after_load},
                {"memory_after_first_queries", after_first_queries},
                {"memory_after_queries", memory()},
                {"index_directory_bytes", stats.directory_bytes},
                {"index_filter_bytes", stats.filter_bytes},
                {"index_cached_bytes", stats.cache_bytes},
                {"index_cached_pages", stats.cached_pages},
                {"index_page_reads", cold_stats.page_reads},
                {"warm_index_page_reads", stats.page_reads - cold_stats.page_reads},
                {"index_cache_hits", stats.cache_hits},
                {"queries_use_bounded_read_session", true},
                {"serialized_index_bytes", 16 + 45 * count}};
    std::cout << result.dump() << '\n';
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
