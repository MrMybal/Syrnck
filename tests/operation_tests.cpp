#include "internal.hpp"
#include <syrnck/git.hpp>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
using namespace syrnck;
namespace {
void check(bool condition) {
  require(condition, "operation progress test failed");
}
template <class F> void cancelled(F function) {
  bool seen = false;
  try {
    function();
  } catch (const Cancelled &) {
    seen = true;
  }
  check(seen);
}
std::string sample(std::size_t size) {
  std::string data(size, '\0');
  std::uint64_t state = 0x123456789abcdef0ULL;
  for (auto &c : data) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    c = static_cast<char>(state);
  }
  return data;
}
} // namespace
int main() {
  auto root = fs::temp_directory_path() / path_from_utf8("syrnck-operations-" + hex(random_bytes(16)));
  fs::create_directory(root);
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code ec;
      fs::remove_all(path, ec);
    }
  } cleanup{root};
  try {
    auto data = sample(5 * 1024 * 1024 + 17);
    for (bool encrypted : {false, true}) {
      std::optional<Key> key;
      if (encrypted) {
        key.emplace();
        key->fill(0x41);
      }
      auto directory = root / (encrypted ? "encrypted" : "plain");
      fs::create_directory(directory);
      Store store(directory / ".syrnck", key, true);
      RepositorySettings settings{4, 4 * 1024 * 1024};
      store.set_settings(settings);
      atomic_write(directory / "asset.bin", bytes(data), false);
      auto options = store.settings().ingest_options();
      std::uint64_t previous = 0;
      bool buffered = false, done = false, path_seen = false;
      auto caller = std::this_thread::get_id();
      options.progress = [&](const ProgressEvent &event) {
        check(std::this_thread::get_id() == caller);
        if (event.phase == ProgressPhase::ingesting) {
          check(event.path == "asset.bin" && event.total_bytes == data.size() && event.bytes >= previous);
          previous = event.bytes;
          buffered |= event.buffered_blocks > 0;
          done |= event.complete;
          path_seen = true;
        }
      };
      auto tip = store.commit_directory(directory, "main", "parallel snapshot", options);
      check(buffered && done && path_seen && previous == data.size());
      auto before = store.refs();
      std::stop_source stop;
      options.stop = stop.get_token();
      options.progress = [&](const ProgressEvent &event) {
        if (event.phase == ProgressPhase::publishing && event.path == "main")
          stop.request_stop();
      };
      cancelled([&] { store.commit_directory(directory, "main", "cancelled snapshot", options); });
      check(store.refs() == before && store.refs().at("heads/main").id == tip);
      cancelled([&] { store.closure({{Kind::revision, tip}}, true, stop.get_token()); });
      cancelled([&] { store.set_ref("new", {Kind::revision, tip}, stop.get_token()); });
      cancelled([&] { store.compare_set_ref("new", {}, {Kind::revision, tip}, stop.get_token()); });
      check(store.refs() == before);
      auto manifest =
          store.metadata(Kind::revision, tip)["files"]["asset.bin"]["manifest"].get<std::string>();
      std::stop_source restore_stop;
      OperationOptions read{restore_stop.get_token(), [&](const ProgressEvent &event) {
                              check(event.phase == ProgressPhase::reconstructing &&
                                    event.total_bytes == data.size());
                              if (event.bytes > 0)
                                restore_stop.request_stop();
                            }};
      auto destination = directory / "restored.bin";
      cancelled([&] { store.restore_file(manifest, destination, read); });
      check(!fs::exists(destination));
      for (const auto &entry : fs::directory_iterator(directory))
        check(!utf8(entry.path().filename()).starts_with(".tmp"));
      store.restore_file(manifest, destination);
      check(text(read_bytes(destination)) == data);
      auto final_destination = directory / "final-cancel.bin";
      std::stop_source final_stop;
      read = {final_stop.get_token(), [&](const ProgressEvent &event) {
                if (event.complete)
                  final_stop.request_stop();
              }};
      cancelled([&] { store.restore_file(manifest, final_destination, read); });
      check(!fs::exists(final_destination));

      Store filtered(root / (encrypted ? "filter-encrypted" : "filter-plain"), key, true);
      filtered.set_settings(settings);
      options = settings.ingest_options();
      buffered = false;
      options.progress = [&](const ProgressEvent &event) { buffered |= event.buffered_blocks > 0; };
      std::istringstream input(data);
      std::ostringstream pointer;
      git::clean(filtered, input, pointer, options);
      check(buffered && git::parse_pointer(pointer.str())->manifest == manifest);
      std::istringstream encoded(pointer.str());
      std::ostringstream output;
      git::smudge(filtered, encoded, output);
      check(output.str() == data);
      before = filtered.refs();
      std::stop_source filter_stop;
      options.stop = filter_stop.get_token();
      options.progress = [&](const ProgressEvent &event) {
        if (event.bytes > 0)
          filter_stop.request_stop();
      };
      std::istringstream retry(data + "modified");
      std::ostringstream no_pointer;
      cancelled([&] { git::clean(filtered, retry, no_pointer, options); });
      check(no_pointer.str().empty() && filtered.refs() == before);
      check(filtered.fsck() > 0 && store.fsck() > 0);
    }
    auto target = root / "atomic.json";
    atomic_write(target, bytes("old"), false);
    std::stop_source stop;
    cancelled([&] {
      atomic_stream(
          target,
          [&](std::ostream &out) {
            out << "new";
            stop.request_stop();
          },
          true, stop.get_token());
    });
    check(text(read_bytes(target)) == "old");
    Store observed(root / "throwing-observer", {}, true);
    IngestOptions options;
    options.workers = 4;
    options.progress = [](const ProgressEvent &event) {
      if (event.buffered_blocks > 0)
        throw Error("observer failed");
    };
    std::istringstream input(data);
    bool reported = false;
    try {
      observed.ingest(input, {}, options);
    } catch (const Error &error) {
      reported = std::string(error.what()) == "observer failed";
    }
    check(reported && observed.refs().empty() && observed.fsck() == 0);
    std::istringstream empty;
    bool complete = false;
    options.progress = [&](const ProgressEvent &event) {
      complete |= event.complete;
      check(event.bytes == 0);
    };
    observed.ingest(empty, {}, options);
    check(complete && observed.fsck() == 1);
    std::cout << "Progress, configured pipelines, cancellation and publication guards passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
