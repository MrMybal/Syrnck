#include <syrnck.h>
#include "native_history_fixture.hpp"
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
using namespace native_test;
using namespace std::chrono_literals;
namespace {
void success(int code) { require(code == 0, syrnck_last_error()); }
struct Cancellation {
  SyrnckCancellation *value = nullptr;
  Cancellation() { success(syrnck_cancellation_create(&value)); }
  ~Cancellation() { syrnck_cancellation_close(value); }
};
struct Session {
  SyrnckWorkspace *value = nullptr;
  Session(const fs::path &project, bool polling) {
    const auto root = utf8(project), storage = utf8(project / ".syrnck");
    const SyrnckWorkspaceOptions options{polling ? 1u : 0u, 4096, 65536};
    success(syrnck_workspace_open_ex(root.c_str(), storage.c_str(), &options, nullptr, &value));
  }
  ~Session() { syrnck_workspace_close(value); }
};
struct OpenStore {
  SyrnckStore *value = nullptr;
  OpenStore(const fs::path &project, const std::optional<Key> &key) {
    const auto storage = utf8(project / ".syrnck");
    success(syrnck_store_open(storage.c_str(), false, key ? key->data() : nullptr, key ? key->size() : 0, &value));
  }
  ~OpenStore() { syrnck_store_close(value); }
};
Json status(Session &session, OpenStore &store, const SyrnckOperation *operation = nullptr, unsigned mode = 0) {
  char *value = nullptr;
  std::size_t length = 0;
  success(syrnck_workspace_status_json_ex(session.value, store.value, nullptr, mode, operation, &value, &length));
  std::unique_ptr<char[], decltype(&syrnck_string_free)> memory(value, syrnck_string_free);
  require(value && length > 1 && value[length - 1] == 0, "invalid cancellable ABI output ownership");
  return Json::parse(value, value + length - 1);
}
struct Progress {
  SyrnckCancellation *cancellation;
  unsigned phase, occurrence, seen = 0, calls = 0;
  bool valid = true, stopped = false;
  static void report(const SyrnckProgress *event, void *user) {
    auto &state = *static_cast<Progress *>(user);
    ++state.calls;
    if (!event || (event->path_length && !event->path) || event->phase > SYRNCK_PHASE_PACKING ||
        event->has_total_bytes > 1 || event->complete > 1 ||
        (event->has_total_bytes && event->bytes > event->total_bytes)) {
      state.valid = false;
      return;
    }
    const std::string_view path(event->path ? event->path : "", event->path_length);
    if (event->phase != state.phase) return;
    if (state.phase == SYRNCK_PHASE_VERIFYING && (path != "large" || event->bytes < 1024 * 1024 || event->complete))
      return;
    if (state.phase == SYRNCK_PHASE_PUBLISHING && path != "working-index") return;
    if (++state.seen == state.occurrence && state.cancellation) {
      state.valid &= syrnck_cancellation_request(state.cancellation) == 0;
      state.stopped = true;
    }
  }
};
void scenario(const fs::path &project, bool encrypted, bool polling) {
  std::optional<Key> key;
  if (encrypted) { key.emplace(); key->fill(67); }
  const std::string content(8 * 1024 * 1024 + 17, 'x');
  Refs references;
  {
    Fixture fixture(project, key);
    fixture.write("large", content);
    fixture.write("asset", "before");
    fixture.store.commit_directory(project, "main", "cancellation baseline");
    fixture.write("asset", "prepared");
    fixture.store.stage_directory(project, "main", {"asset"});
    references = fixture.store.refs();
  }
  Session session(project, polling);
  {
    OpenStore store(project, key);
    const auto initial = status(session, store);
    require(initial["observation"]["full_scan"] == true, "initial cancellable status was not full");
    for (const auto &[phase, occurrence] : {std::pair{SYRNCK_PHASE_SCANNING, 1u},
                                          std::pair{SYRNCK_PHASE_VERIFYING, 1u},
                                          std::pair{SYRNCK_PHASE_PUBLISHING, 2u}}) {
      success(syrnck_workspace_invalidate(session.value));
      Cancellation cancellation;
      Progress progress{cancellation.value, static_cast<unsigned>(phase), occurrence};
      const SyrnckOperation operation{cancellation.value, Progress::report, &progress};
      char *value = reinterpret_cast<char *>(1);
      std::size_t length = 123;
      const auto result = syrnck_workspace_status_json_ex(session.value, store.value, nullptr, 2, &operation, &value, &length);
      require(result == SYRNCK_CANCELLED && !value && !length && progress.valid && progress.stopped && progress.calls,
              "C progress cancellation returned partial/stale status or missed its checkpoint");
      require(std::string(syrnck_last_error()) == "operation cancelled", "C cancellation diagnostic differs");
      const auto calls = progress.calls;
      const auto recovered = status(session, store, nullptr, 2);
      require(recovered["unstaged"]["observation"]["full_scan"] == true &&
                  recovered["unstaged"]["hashed_bytes"] == content.size() + 8 && progress.calls == calls,
              "cancelled status retained inventory or retained its progress callback");
      require(syrnck_workspace_status_json_ex(session.value, store.value, nullptr, 0, &operation, &value, &length) ==
                  SYRNCK_CANCELLED && !value && !length && progress.calls == calls,
              "requested cancellation source was silently reset for a later operation");
      status(session, store);
    }
    Progress progress{nullptr, SYRNCK_PHASE_VERIFYING, 1};
    const SyrnckOperation operation{nullptr, Progress::report, &progress};
    success(syrnck_workspace_invalidate(session.value));
    require(status(session, store, &operation)["hashed_bytes"] == content.size() + 8 && progress.valid && progress.calls,
            "progress without cancellation did not preserve complete status");
  }
  {
    Cancellation cancellation;
    success(syrnck_cancellation_request(cancellation.value));
    success(syrnck_cancellation_request(cancellation.value));
    SyrnckWorkspace *unused = reinterpret_cast<SyrnckWorkspace *>(1);
    const auto root = utf8(project), storage = utf8(project / ".syrnck");
    require(syrnck_workspace_open_ex(root.c_str(), storage.c_str(), nullptr, cancellation.value, &unused) ==
                SYRNCK_CANCELLED && !unused, "pre-cancelled registration leaked an armed session");
    char *value = reinterpret_cast<char *>(1);
    std::size_t length = 123;
    require(syrnck_workspace_wait_json_ex(session.value, 60000, cancellation.value, &value, &length) ==
                SYRNCK_CANCELLED && !value && !length, "pre-cancelled wait exposed partial output");
  }
  // No working data/reference/staging mutation is authorized by observation.
  {
    Store store(project / ".syrnck", key);
    require(store.refs() == references && text(read_bytes(project / "large")) == content &&
                text(read_bytes(project / "asset")) == "prepared", "cancellation changed retained or working data");
    store.gc(true);
    require(store.refs() == references && store.fsck() > 0, "cancellation damaged retained data after GC");
  }
  {
    OpenStore store(project, key);
    require(status(session, store)["observation"]["full_scan"] == true, "cancelled wait did not require full recovery");
  }
}
void idle_wait(const fs::path &project, bool polling) {
  { Store store(project / ".syrnck", {}, true); }
  Session session(project, polling);
  { OpenStore store(project, {}); status(session, store); }
  Cancellation cancellation;
  std::atomic<bool> valid = true;
  std::jthread request([&] {
    std::this_thread::sleep_for(30ms);
    valid = syrnck_cancellation_request(cancellation.value) == 0;
  });
  char *value = nullptr;
  std::size_t length = 123;
  const auto start = std::chrono::steady_clock::now();
  require(syrnck_workspace_wait_json_ex(session.value, 3000, cancellation.value, &value, &length) ==
              SYRNCK_CANCELLED && !value && !length && std::chrono::steady_clock::now() - start < 1s,
          "native idle wait did not cooperate with a concurrent stop request");
  request.join();
  require(valid, "concurrent stop request failed");
}
void concurrent_requests() {
  Cancellation cancellation;
  require(syrnck_cancellation_request(nullptr) == 1, "null stop request was accepted");
  const auto original = std::string(syrnck_last_error());
  std::atomic<bool> valid = true;
  {
    std::vector<std::jthread> workers;
    for (unsigned i = 0; i != 8; ++i)
      workers.emplace_back([&] {
        for (unsigned j = 0; j != 100; ++j)
          if (syrnck_cancellation_request(cancellation.value) != 0) valid = false;
      });
  }
  require(valid && original == syrnck_last_error(), "concurrent requests corrupted thread-local diagnostics");
}
} // namespace
int main() {
  Temporary temporary;
  try {
    concurrent_requests();
    for (bool encrypted : {false, true})
      for (bool polling : {false, true})
        scenario(temporary.path / (std::to_string(encrypted) + '-' + std::to_string(polling)), encrypted, polling);
    idle_wait(temporary.path / "native-idle", false);
    idle_wait(temporary.path / "polling-idle", true);
    std::cout << "C workspace cancellation, progress checkpoints, recovery and retained data passed\n";
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
