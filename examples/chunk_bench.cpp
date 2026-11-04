#include <syrnck/core.hpp>
#include <chrono>
#include <iostream>
#include <sstream>
using namespace syrnck;
int main(int argc, char **argv) {
  try {
    std::size_t mib = argc > 1 ? std::stoull(argv[1]) : 64;
    require(mib >= 4 && mib <= 1024, "choose 4..1024 MiB");
    IngestOptions options;
    options.workers = argc > 2 ? std::stoull(argv[2]) : 1;
    bool encrypted = argc > 3 && std::string(argv[3]) == "1";
    auto buffer_mib = argc > 4 ? std::stoull(argv[4]) : 32;
    require(buffer_mib >= 4 && buffer_mib <= 1024, "choose a 4..1024 MiB buffer budget");
    options.max_in_flight_bytes = static_cast<std::size_t>(buffer_mib * 1024 * 1024);
    auto pattern = argc > 5 ? std::string(argv[5]) : "random";
    require(pattern == "random" || pattern == "compressible", "choose random or compressible");
    std::string original(mib * 1024 * 1024, '\0');
    std::uint64_t state = 0x123456789abcdef0ULL;
    for (auto &c : original) {
      state ^= state << 13;
      state ^= state >> 7;
      state ^= state << 17;
      c = static_cast<char>(state);
    }
    if (pattern == "compressible")
      for (std::size_t i = 0; i < original.size(); ++i)
        if (i % 64 != 0)
          original[i] = static_cast<char>(i % 7);
    auto edited = original;
    edited.insert(1024 * 1024, "a new variable-length asset header");
    for (bool fixed : {false, true}) {
      auto root = fs::temp_directory_path() / path_from_utf8("syrnck-bench-" + hex(random_bytes(16)));
      struct Cleanup {
        fs::path p;
        ~Cleanup() {
          std::error_code ec;
          fs::remove_all(p, ec);
        }
      } cleanup{root};
      std::optional<Key> key;
      if (encrypted) {
        key.emplace();
        key->fill(7); // Public benchmark key; never use it for private data.
      }
      Store store(root, key, true);
      auto policy = fixed ? Chunking::fixed_size(1024 * 1024) : Chunking{};
      using Clock = std::chrono::steady_clock;
      std::istringstream source(original);
      auto started = Clock::now();
      auto [initial_id, initial] = store.ingest(source, policy, options);
      auto initial_seconds = std::chrono::duration<double>(Clock::now() - started).count();
      std::istringstream changed(edited);
      started = Clock::now();
      auto [id, delta] = store.ingest(changed, policy, options);
      auto edited_seconds = std::chrono::duration<double>(Clock::now() - started).count();
      std::ostringstream output;
      started = Clock::now();
      store.reconstruct(id, output);
      auto restore_seconds = std::chrono::duration<double>(Clock::now() - started).count();
      require(output.str() == edited, "reconstruction differs");
      std::cout << Json{{"scenario", "insertion-in-synthetic-data"},
                        {"implementation", "cpp20"},
                        {"pattern", pattern},
                        {"workers", options.workers},
                        {"encrypted", encrypted},
                        {"buffer_mib", buffer_mib},
                        {"policy", fixed ? "fixed-1mib" : "fastcdc2020"},
                        {"mib", mib},
                        {"initial_manifest", initial_id},
                        {"edited_manifest", id},
                        {"initial", initial.json()},
                        {"delta", delta.json()},
                        {"initial_mib_per_second", static_cast<double>(mib) / initial_seconds},
                        {"edited_mib_per_second", static_cast<double>(mib) / edited_seconds},
                        {"restore_mib_per_second", static_cast<double>(mib) / restore_seconds},
                        {"byte_exact_restore", true}}
                       .dump()
                << '\n';
    }
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
