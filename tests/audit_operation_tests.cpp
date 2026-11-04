#include "internal.hpp"
#include <iostream>
#include <sstream>
#include <thread>
using namespace syrnck;
namespace {
void check(bool condition) {
  require(condition, "audit operation test failed");
}
template <class Function> void cancelled(Function function) {
  bool seen = false;
  try {
    function();
  } catch (const Cancelled &) {
    seen = true;
  }
  check(seen);
}
fs::path loose(const Store &store, const ObjectKey &key) {
  return store.root() / "objects" / kind_name(key.kind) / key.id.substr(0, 2) / key.id;
}
} // namespace
int main() {
  auto root = fs::temp_directory_path() / path_from_utf8("syrnck-audit-operation-" + hex(random_bytes(16)));
  fs::create_directory(root);
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code error;
      fs::remove_all(path, error);
    }
  } cleanup{root};
  try {
    std::string data(2 * 1024 * 1024 + 17, '\0');
    std::uint64_t state = 1234567;
    for (auto &c : data) {
      state ^= state << 13;
      state ^= state >> 7;
      state ^= state << 17;
      c = static_cast<char>(state);
    }
    for (bool encrypted : {false, true})
      for (int layout = 0; layout < 3; ++layout) {
        std::optional<Key> secret;
        if (encrypted) {
          secret.emplace();
          secret->fill(0x61);
        }
        Store store(root / (std::to_string(encrypted) + "-" + std::to_string(layout)), secret, true);
        std::istringstream source(data);
        ObjectKey manifest{Kind::manifest, store.ingest(source, Chunking::fixed_size(65536)).first};
        store.set_ref("retained", manifest);
        auto chunk = store.objects().front();
        auto envelope = read_bytes(loose(store, chunk));
        if (layout) {
          store.pack({.max_objects_per_pack = 8});
          if (layout == 2)
            atomic_write(loose(store, chunk), envelope, false);
        }
        auto objects = store.objects();
        auto references = store.refs();
        auto physical = read_bytes(store.root() / "config.json");
        auto caller = std::this_thread::get_id();
        std::uint64_t previous = 0;
        bool complete = false;
        OperationOptions options{{}, [&](const ProgressEvent &event) {
                                   check(event.phase == ProgressPhase::verifying && event.path.empty() &&
                                         !event.total_bytes);
                                   check(event.bytes >= previous && caller == std::this_thread::get_id());
                                   previous = event.bytes;
                                   complete |= event.complete;
                                 }};
        check(store.fsck(options) == objects.size() && complete && previous >= data.size());
        std::stop_source stop;
        options.stop = stop.get_token();
        options.progress = [&](const ProgressEvent &event) {
          if (event.bytes > 0)
            stop.request_stop();
        };
        cancelled([&] { store.fsck(options); });
        cancelled([&] { store.fsck(options); }); // Already stopped before touching the inventory.
        check(store.objects() == objects && store.refs() == references &&
              read_bytes(store.root() / "config.json") == physical);
        std::stop_source traversal;
        cancelled([&] {
          store.closure({manifest}, true, traversal.get_token(), [&](const ProgressEvent &event) {
            if (event.bytes > 0)
              traversal.request_stop();
          });
        });
        bool reported = false;
        try {
          store.fsck({{}, [](const ProgressEvent &event) {
                        if (event.bytes > 0)
                          throw Error("audit observer failed");
                      }});
        } catch (const Error &error) {
          reported = std::string(error.what()) == "audit observer failed";
        }
        check(reported && store.fsck() == objects.size());
        // An interrupted audit must restore normal cache use and close its pack session.
        store.clear_cache();
        store.reset_io_stats();
        (void)store.get(chunk);
        (void)store.get(chunk);
        check(store.io_stats().cache_data_hits > 0);
        check(store.pack({.max_objects_per_pack = 8, .compact = true})["compacted"] == true);
        check(store.fsck() == objects.size());
      }
    std::cout << "Cancellable streaming audits and closures passed in six storage layouts\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
