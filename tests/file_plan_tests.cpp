#include <syrnck/core.hpp>
#include <fstream>
#include <iostream>
#include <sstream>
using namespace syrnck;
namespace {
auto view(const std::string &s) {
  return std::span(reinterpret_cast<const std::uint8_t *>(s.data()), s.size());
}
std::string ingest(const Store &store, const std::string &value, std::uint32_t size = 1024) {
  std::istringstream input(value);
  return store.ingest(input, Chunking::fixed_size(size)).first;
}
template <class F> void fails(F function) {
  bool failed = false;
  try {
    function();
  } catch (const Error &) {
    failed = true;
  }
  require(failed, "expected plan failure");
}
} // namespace
int main() {
  auto root = fs::temp_directory_path() / ("syrnck-plans-" + hex(random_bytes(16)));
  struct Cleanup {
    fs::path root;
    ~Cleanup() {
      std::error_code e;
      fs::remove_all(root, e);
    }
  } cleanup{root};
  try {
    Store store(root / "store", {}, true);
    const std::string a(1024, 'a'), b(1024, 'b'), c(1024, 'c'), d(1024, 'd');
    const auto original = a + b + a + c, changed = c + a + d + d + b + "tail";
    const auto source = ingest(store, original), target = ingest(store, changed);
    store.reset_io_stats();
    const auto plan = store.compare_files(source, target);
    require(store.io_stats().object_reads == 2, "comparison read payload objects");
    require(plan["reused_bytes"] == 3072 && plan["new_bytes"] == 2052 && plan["unique_new_bytes"] == 1028,
            "reuse byte accounting differs");
    require(plan["blocks"][0]["source_offset"] == 3072 && plan["blocks"][1]["source_offset"] == 0 &&
                plan["blocks"][4]["source_offset"] == 1024 && plan["blocks"][2]["source_offset"].is_null(),
            "moved or repeated source blocks differ");
    require(plan["needed_chunks"].size() == 2, "new chunk deduplication differs");
    std::string reconstructed;
    for (const auto &block : plan["blocks"]) {
      if (block["source_offset"].is_null()) {
        const auto bytes = store.get({Kind::chunk, block["id"].get<std::string>()});
        reconstructed.append(reinterpret_cast<const char *>(bytes.data()), bytes.size());
      } else
        reconstructed +=
            original.substr(block["source_offset"].get<std::size_t>(), block["size"].get<std::size_t>());
    }
    require(reconstructed == changed, "plan did not reconstruct target bytes");
    store.reset_io_stats();
    auto identical = store.compare_files(source, source);
    require(identical["same_content"] == true && identical["new_bytes"] == 0 &&
                store.io_stats().object_reads == 1,
            "identical plan differs");
    const auto rechunked = ingest(store, original, 512);
    auto same = store.compare_files(source, rechunked);
    require(same["same_content"] == true && same["reused_bytes"] == 0,
            "content equality was confused with chunk equality");
    const auto empty = ingest(store, "");
    require(store.compare_files(source, empty)["blocks"].empty(), "empty target differs");
    require(store.compare_files(empty, source)["unique_new_bytes"] == 3072,
            "empty source deduplication differs");
    auto bad = store.metadata(Kind::manifest, source);
    bad["blocks"][0]["size"] = 1023;
    bad["size"] = bad["size"].get<std::uint64_t>() - 1;
    const auto inconsistent = store.put(Kind::manifest, view(bad.dump())).first.id;
    fails([&] { (void)store.compare_files(inconsistent, target); });
    auto cross = store.metadata(Kind::manifest, target);
    cross["blocks"][0]["size"] = 1023;
    cross["size"] = cross["size"].get<std::uint64_t>() - 1;
    const auto cross_id = store.put(Kind::manifest, view(cross.dump())).first.id;
    fails([&] { (void)store.compare_files(source, cross_id); });
    auto repeated = cross;
    repeated["blocks"][2]["size"] = 1023;
    repeated["size"] = repeated["size"].get<std::uint64_t>() - 1;
    const auto repeated_id = store.put(Kind::manifest, view(repeated.dump())).first.id;
    fails([&] { (void)store.compare_files(empty, repeated_id); });
    std::stop_source stop;
    fails([&] {
      (void)store.compare_files(source, target,
                                {stop.get_token(), [&](const ProgressEvent &) { stop.request_stop(); }});
    });
    require(store.refs().empty(), "comparison changed references");
    // A metadata plan remains available even when payloads are not resident.
    auto blocks = store.metadata(Kind::manifest, source)["blocks"];
    auto id = blocks[0]["id"].get<std::string>();
    fs::remove(store.root() / "objects/chunk" / id.substr(0, 2) / id);
    require(store.compare_files(source, target) == plan, "metadata plan required source payload");
    std::cout << "Verified reuse plans: moved/repeated chunks, deduplication, content equality, "
                 "metadata-only reads and cancellation\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
