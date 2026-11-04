#include <syrnck/core.hpp>
#include <chrono>
#include <iostream>
#include <fstream>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#endif
using namespace syrnck;
namespace {
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
} // namespace
int main(int argc, char **argv) {
  try {
    require(argc == 2, "usage: compact_bench DIRECTORY");
    auto root = path_from_utf8(argv[1]);
    std::ifstream input(root / "fixture.json", std::ios::binary);
    require(input.is_open(), "missing benchmark fixture");
    auto expected = Json::parse(input);
    Store store(root, {}, false, {.decoded_cache_bytes = 0, .verified_cache_entries = 0});
    auto refs = store.refs();
    auto before = memory();
    auto start = std::chrono::steady_clock::now();
    auto result = store.pack({.max_objects_per_pack = 65536, .compact = true});
    auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    auto after = memory();
    auto io = store.io_stats().json();
    require(result["objects"] == expected["objects"] && result["objects_written"] == expected["objects"],
            "compaction object count differs");
    require(store.refs() == refs && store.fsck() == expected["objects"].get<std::size_t>(),
            "post-compaction reference or full-file audit failed");
    std::cout << Json{{"seconds", seconds},          {"memory_before", before},
                      {"memory_after", after},       {"result", result},
                      {"io_before_post_audit", io},  {"all_file_hashes_verified_after_compaction", true},
                      {"references_unchanged", true}}
                     .dump()
              << '\n';
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
