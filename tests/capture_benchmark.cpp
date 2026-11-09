#include "native_history_fixture.hpp"
#include <syrnck/workspace.hpp>
#include <chrono>
#include <iostream>
#include <thread>
using namespace native_test;
int main(int argc, char **argv) {
  try {
    require(argc == 5, "usage: capture_benchmark MIB FILES ITERATIONS ENCRYPTED");
    const auto mib = std::stoull(argv[1]), files = std::stoull(argv[2]), iterations = std::stoull(argv[3]);
    require(mib >= 1 && mib <= 4096 && files >= 1 && files <= 4096 && iterations >= 1 && iterations <= 100,
            "invalid capture benchmark limits");
    std::optional<Key> key;
    if (std::string_view(argv[4]) == "1") { key.emplace(); key->fill(69); }
    Temporary temporary;
    std::unique_ptr<WorkspaceCapture> incremental;
    const auto file_size = mib * 1024 * 1024 / files;
    const auto total = file_size * files;
    for (const auto *mode : {"full", "incremental"}) {
      const auto project = temporary.path / mode;
      Fixture fixture(project, key);
      std::uint64_t state = 0x987654321abcdef;
      for (std::uint64_t i = 0; i < files; ++i) {
        Bytes data(file_size);
        for (auto &byte : data) { state ^= state << 13; state ^= state >> 7; state ^= state << 17; byte = static_cast<std::uint8_t>(state); }
        fs::create_directories(project / "Assets");
        atomic_write(project / "Assets" / ("asset" + std::to_string(i) + ".bin"), data, false);
      }
      fixture.write("code.cpp", "initial code");
      if (std::string_view(mode) == "incremental") {
        incremental = std::make_unique<WorkspaceCapture>(project, fixture.store.root());
        incremental->capture(fixture.store, "main", "warm", fixture.store.settings().ingest_options());
      } else fixture.store.commit_directory(project, "main", "warm");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(3100));
    Json rows = Json::array();
    for (std::uint64_t n = 0; n < iterations; ++n) {
      // Alternate order; timings include Store open, complete publication and
      // immutable graph verification. Counters separate source from object I/O.
      const std::vector<std::string> order = n % 2 ? std::vector<std::string>{"incremental", "full"}
                                                 : std::vector<std::string>{"full", "incremental"};
      for (const auto &mode : order) {
        const auto project = temporary.path / mode;
        const auto code = "code iteration " + std::to_string(n);
        atomic_write(project / "code.cpp", syrnck::bytes(code), true);
        const auto start = std::chrono::steady_clock::now();
        Store store(project / ".syrnck", key);
        auto options = store.settings().ingest_options();
        std::uint64_t source_read = 0;
        options.progress = [&](const ProgressEvent &event) {
          if (event.phase == ProgressPhase::ingesting && event.complete) source_read += event.bytes;
        };
        std::string revision; Json capture;
        if (mode == "incremental") {
          const auto result = incremental->capture(store, "main", "benchmark", options);
          revision = result["revision"].get<std::string>(); capture = result["capture"];
          require(capture["source_bytes_read"] == source_read &&
              capture["source_bytes_read"].get<std::uint64_t>() + capture["source_bytes_reused"].get<std::uint64_t>() == total + code.size(),
              "capture benchmark counters differ from actual source reads");
        } else {
          revision = store.commit_directory(project, "main", "benchmark", options);
          require(source_read == total + code.size(), "full capture did not read its source");
        }
        const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const auto io = store.io_stats().json();
        require(store.status_directory(project, revision)["clean"].get<bool>(), "benchmark revision differs from working bytes");
        rows.push_back(Json{{"mode", mode}, {"iteration", n + 1}, {"seconds", seconds},
            {"source_bytes_read", source_read}, {"capture", capture}, {"store_io", io}, {"byte_exact_capture", true}});
      }
    }
    std::cout << Json{{"version", 1}, {"asset_bytes", total}, {"files", files}, {"encrypted", key.has_value()},
        {"iterations", iterations}, {"rows", rows}}.dump(2) << '\n';
    return 0;
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
