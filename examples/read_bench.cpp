#include <syrnck/core.hpp>
#include <chrono>
#include <iostream>
#include <sstream>
using namespace syrnck;

int main(int argc, char **argv) {
  try {
    auto mib = argc > 1 ? std::stoull(argv[1]) : 32;
    auto cache_mib = argc > 2 ? std::stoull(argv[2]) : 64;
    bool encrypted = argc > 3 && std::string(argv[3]) == "1";
    auto pattern = argc > 4 ? std::string(argv[4]) : "random";
    require(mib >= 4 && mib <= 256 && cache_mib <= 1024, "choose 4..256 MiB and 0..1024 MiB cache");
    require(pattern == "random" || pattern == "repeated", "choose random or repeated");
    std::string data(static_cast<std::size_t>(mib * 1024 * 1024), '\0');
    std::uint64_t state = 0x123456789abcdef0ULL;
    for (auto &byte : data) {
      state ^= state << 13;
      state ^= state >> 7;
      state ^= state << 17;
      byte = static_cast<char>(state);
    }
    if (pattern == "repeated")
      for (std::size_t i = 1024 * 1024; i < data.size(); ++i)
        data[i] = data[i % (1024 * 1024)];
    auto root = fs::temp_directory_path() / path_from_utf8("syrnck-read-bench-" + hex(random_bytes(16)));
    struct Cleanup {
      fs::path path;
      ~Cleanup() {
        std::error_code error;
        fs::remove_all(path, error);
      }
    } cleanup{root};
    std::optional<Key> key;
    if (encrypted) {
      key.emplace();
      key->fill(7);
    }
    StoreOptions options;
    options.decoded_cache_bytes = static_cast<std::size_t>(cache_mib * 1024 * 1024);
    if (cache_mib == 0)
      options.verified_cache_entries = 0;
    Store store(root, key, true, options);
    auto chunking = Chunking::fixed_size(1024 * 1024);
    std::istringstream input(data);
    auto id = store.ingest(input, chunking).first;
    ObjectKey object{Kind::manifest, id};
    store.set_ref("warm-up", object);
    auto manifest = store.metadata(Kind::manifest, id);
    Json stages = Json::object();
    using Clock = std::chrono::steady_clock;
    auto measure = [&](std::string_view name, const auto &function) {
      store.reset_io_stats();
      auto started = Clock::now();
      function();
      auto seconds = std::chrono::duration<double>(Clock::now() - started).count();
      stages[std::string(name)] = Json{{"seconds", seconds},
                                       {"mib_per_second", static_cast<double>(mib) / seconds},
                                       {"io", store.io_stats().json()}};
    };
    measure("presence", [&] {
      for (const auto &block : manifest["blocks"])
        require(store.contains({Kind::chunk, block["id"].get<std::string>()}), "block disappeared");
    });
    measure("reingest", [&] {
      std::istringstream same(data);
      auto [again, stats] = store.ingest(same, chunking);
      require(again == id && stats.new_bytes == 0, "reingestion differs");
    });
    measure("publication", [&] { store.set_ref("verified-version", object); });
    measure("reconstruction", [&] {
      std::ostringstream output;
      store.reconstruct(id, output);
      require(output.str() == data, "restored bytes differ");
    });
    auto physical_objects = store.objects().size();
    measure("fsck", [&] { require(store.fsck() == physical_objects, "object count differs"); });
    auto audit = store.io_stats();
    require(audit.cache_data_hits == 0 && audit.cache_presence_hits == 0, "fsck used cached validation");
    std::cout << Json{{"implementation", "cpp20"},
                      {"scenario", "warm-verified-reads"},
                      {"mib", mib},
                      {"cache_mib", cache_mib},
                      {"encrypted", encrypted},
                      {"pattern", pattern},
                      {"manifest", id},
                      {"physical_objects", physical_objects},
                      {"byte_exact_restore", true},
                      {"stages", stages}}
                     .dump()
              << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
