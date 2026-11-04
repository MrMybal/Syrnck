#include <syrnck/core.hpp>
#include "../src/encode_pool.hpp"
#include <algorithm>
#include <atomic>
#include <fstream>
#include <iostream>
#include <sstream>
using namespace syrnck;

namespace {
void check(bool value, std::string_view message = "ingest test failed") {
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
std::size_t manifests(const Store &store) {
  auto objects = store.objects();
  return static_cast<std::size_t>(std::count_if(objects.begin(), objects.end(),
                                                [](const auto &key) { return key.kind == Kind::manifest; }));
}
struct ControlledInput : std::stringbuf {
  std::function<void(std::size_t)> on_read;
  std::size_t reads = 0;
  ControlledInput(const std::string &value, std::function<void(std::size_t)> callback)
      : std::stringbuf(value), on_read(std::move(callback)) {}
  std::streamsize xsgetn(char *destination, std::streamsize size) override {
    on_read(++reads);
    return std::stringbuf::xsgetn(destination, size);
  }
};
} // namespace

int main() {
  auto root = fs::temp_directory_path() / path_from_utf8("syrnck-ingest-" + hex(random_bytes(16)));
  fs::create_directory(root);
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code error;
      fs::remove_all(path, error);
    }
  } cleanup{root};
  std::size_t count = 0;
  auto test = [&](std::string_view name, const auto &function) {
    function();
    ++count;
    std::cout << "PASS " << name << '\n';
  };
  try {
    auto data = sample(12 * 1024 * 1024 + 37);
    test("parallel IDs, manifests and reconstruction match sequential ingestion", [&] {
      for (bool encrypted : {false, true}) {
        for (bool fixed : {false, true}) {
          auto suffix = std::string(encrypted ? "encrypted-" : "plain-") + (fixed ? "fixed" : "cdc");
          std::optional<Key> key;
          if (encrypted) {
            key.emplace();
            key->fill(71);
          }
          Store sequential(root / (suffix + "-sequential"), key, true);
          Store parallel(root / (suffix + "-parallel"), key, true);
          auto chunking = fixed ? Chunking::fixed_size(65536) : Chunking{};
          IngestOptions options;
          options.workers = 4;
          options.max_in_flight_bytes = 8 * 1024 * 1024;
          std::istringstream first(data), second(data);
          auto [expected, serial_stats] = sequential.ingest(first, chunking);
          auto [actual, stats] = parallel.ingest(second, chunking, options);
          check(expected == actual, "parallel ingestion changed object IDs");
          check(sequential.metadata(Kind::manifest, expected) == parallel.metadata(Kind::manifest, actual));
          check(stats.file_bytes == serial_stats.file_bytes && stats.new_bytes == serial_stats.new_bytes &&
                stats.reused_bytes == serial_stats.reused_bytes && stats.blocks == serial_stats.blocks);
          check(stats.peak_in_flight_bytes > 0 && stats.peak_in_flight_bytes <= options.max_in_flight_bytes);
          std::ostringstream restored;
          parallel.reconstruct(actual, restored);
          check(restored.str() == data, "parallel reconstruction differs");
          std::istringstream again(data);
          auto [reused, repeated] = parallel.ingest(again, chunking, options);
          check(reused == actual && repeated.new_bytes == 0 && repeated.reused_bytes == data.size());
          check(repeated.peak_in_flight_bytes == 0, "existing data should not be recompressed");
        }
      }
    });
    test("repeated pending blocks are submitted and accounted once", [&] {
      Store store(root / "repeated", {}, true);
      auto block = sample(65536);
      std::string repeated;
      for (int i = 0; i < 64; ++i)
        repeated += block;
      std::istringstream input(repeated);
      IngestOptions options;
      options.workers = 8;
      options.max_in_flight_bytes = 65536;
      auto [id, stats] = store.ingest(input, Chunking::fixed_size(65536), options);
      check(stats.blocks == 64 && stats.new_bytes == 65536 && stats.reused_bytes == repeated.size() - 65536);
      check(stats.peak_in_flight_bytes == 65536 && store.objects().size() == 2);
      std::ostringstream out;
      store.reconstruct(id, out);
      check(out.str() == repeated);
    });
    test("queue budget smaller than the stream applies backpressure", [&] {
      Store store(root / "bounded", {}, true);
      IngestOptions options;
      options.workers = 4;
      options.max_in_flight_bytes = 2 * 65536;
      std::istringstream input(data);
      auto [id, stats] = store.ingest(input, Chunking::fixed_size(65536), options);
      check(stats.peak_in_flight_bytes == options.max_in_flight_bytes);
      check(stats.file_bytes > options.max_in_flight_bytes * 32);
      std::ostringstream out;
      store.reconstruct(id, out);
      check(out.str() == data);
    });
    test("tiny chunks cannot create an unbounded number of queued tasks", [&] {
      Store store(root / "tiny", {}, true);
      std::string tiny;
      for (int i = 0; i < 256; ++i)
        tiny.push_back(static_cast<char>(i));
      IngestOptions options;
      options.workers = 3;
      std::istringstream input(tiny);
      auto [id, stats] = store.ingest(input, Chunking::fixed_size(1), options);
      check(stats.blocks == 256 && stats.peak_in_flight_blocks == 2 * options.workers);
      check(stats.peak_in_flight_bytes == stats.peak_in_flight_blocks);
      std::ostringstream output;
      store.reconstruct(id, output);
      check(output.str() == tiny);
    });
    test("read failure preserves refs and never publishes a manifest", [&] {
      Store store(root / "read-failure", {}, true);
      std::istringstream prior("retained version");
      auto id = store.ingest(prior).first;
      store.set_ref("release", {Kind::manifest, id});
      auto before = store.refs();
      ControlledInput buffer(data, [](std::size_t read) {
        if (read == 5)
          throw Error("injected input failure");
      });
      std::istream input(&buffer);
      IngestOptions options;
      options.workers = 4;
      options.max_in_flight_bytes = 2 * 65536;
      fails([&] { store.ingest(input, Chunking::fixed_size(65536), options); });
      check(manifests(store) == 1 && store.refs() == before);
      check(store.gc().at("unreachable_objects").get<std::size_t>() > 0);
      store.gc(true);
      std::ostringstream out;
      store.reconstruct(id, out);
      check(out.str() == "retained version");
    });
    test("cancellation drains workers without publishing a manifest", [&] {
      Store store(root / "cancelled", {}, true);
      std::stop_source stop;
      ControlledInput buffer(data, [&](std::size_t read) {
        if (read == 5)
          stop.request_stop();
      });
      std::istream input(&buffer);
      IngestOptions options;
      options.workers = 4;
      options.max_in_flight_bytes = 2 * 65536;
      options.stop = stop.get_token();
      bool cancelled = false;
      try {
        store.ingest(input, Chunking::fixed_size(65536), options);
      } catch (const Cancelled &) {
        cancelled = true;
      }
      check(cancelled && manifests(store) == 0 && store.refs().empty());
      std::istringstream retry("retry after cancellation");
      check(!store.ingest(retry).first.empty());
      auto before = store.objects().size();
      std::istringstream empty;
      fails([&] { store.ingest(empty, {}, options); });
      check(store.objects().size() == before);
    });
    test("publication failure joins workers and is recoverable", [&] {
      Store store(root / "write-failure", {}, true);
      auto blocked = store.root() / "objects/chunk";
      {
        std::ofstream file(blocked);
        file << "not a directory";
      }
      IngestOptions options;
      options.workers = 4;
      options.max_in_flight_bytes = 2 * 65536;
      std::istringstream input(data);
      fails([&] { store.ingest(input, Chunking::fixed_size(65536), options); });
      check(!fs::exists(store.root() / "objects/manifest") && store.refs().empty());
      fs::remove(blocked);
      std::istringstream retry(data);
      auto [id, stats] = store.ingest(retry, Chunking::fixed_size(65536), options);
      check(stats.file_bytes == data.size());
      std::ostringstream out;
      store.reconstruct(id, out);
      check(out.str() == data);
    });
    test("worker exceptions are returned to the caller", [&] {
      std::atomic<int> calls = 0;
      EncodePool pool(4, [&](ZSTD_CCtx *, const ObjectKey &, std::span<const std::uint8_t>) -> Bytes {
        ++calls;
        throw Error("injected encoder failure");
      });
      std::vector<std::future<Bytes>> futures;
      for (int i = 0; i < 16; ++i)
        futures.push_back(pool.submit({Kind::chunk, std::string(64, '0')}, Bytes{1}));
      for (auto &future : futures) {
        bool reported = false;
        try {
          (void)future.get();
        } catch (const Error &error) {
          reported = std::string(error.what()) == "injected encoder failure";
        }
        check(reported, "worker exception was lost or replaced");
      }
      check(calls == 16);
    });
    test("v1 metadata limit is enforced before consuming an overfragmented file", [&] {
      Store store(root / "metadata-limit", {}, true);
      std::string fragmented(1024 * 1024, 'A');
      std::istringstream input(fragmented);
      IngestOptions options;
      options.workers = 4;
      bool limited = false;
      try {
        store.ingest(input, Chunking::fixed_size(1), options);
      } catch (const Error &error) {
        limited = std::string(error.what()) == "manifest exceeds the v1 metadata size limit";
      }
      check(limited, "expected the v1 metadata limit");
      check(input.tellg() > 0 && input.tellg() < static_cast<std::streampos>(fragmented.size()));
      check(manifests(store) == 0 && store.refs().empty());
    });
    test("empty files and invalid limits do not launch work", [&] {
      Store store(root / "limits", {}, true);
      IngestOptions options;
      options.workers = 4;
      std::istringstream empty;
      auto [id, stats] = store.ingest(empty, {}, options);
      check(stats.blocks == 0 && stats.peak_in_flight_bytes == 0);
      check(store.metadata(Kind::manifest, id).at("blocks").empty());
      auto before = store.objects().size();
      for (auto workers : {0U, 65U}) {
        options.workers = workers;
        std::istringstream input(data);
        fails([&] { store.ingest(input, {}, options); });
      }
      options.workers = 4;
      for (auto bytes : {0ULL, 1024ULL, 1024ULL * 1024 * 1024 + 1}) {
        options.max_in_flight_bytes = static_cast<std::size_t>(bytes);
        std::istringstream input(data);
        fails([&] { store.ingest(input, {}, options); });
      }
      check(store.objects().size() == before);
    });
    std::cout << count << " pipeline checks passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAIL " << e.what() << '\n';
    return 1;
  }
}
