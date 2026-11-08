#include <syrnck.h>
#include <syrnck/watcher.hpp>
#include "native_history_fixture.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
using namespace native_test;
using namespace std::chrono_literals;
namespace {
void success(int code) { require(code == 0, syrnck_last_error()); }
struct Session {
  SyrnckWorkspace *value = nullptr;
  Session(const fs::path &project, bool polling, std::uint32_t paths = 4096) {
    const auto root = utf8(project), storage = utf8(project / ".syrnck");
    const SyrnckWorkspaceOptions options{polling ? 1u : 0u, paths, 65536};
    success(syrnck_workspace_open(root.c_str(), storage.c_str(), !polling && paths == 4096 ? nullptr : &options, &value));
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
Json output(const std::function<int(char **, std::size_t *)> &call) {
  char *value = nullptr;
  std::size_t length = 0;
  success(call(&value, &length));
  std::unique_ptr<char[], decltype(&syrnck_string_free)> memory(value, syrnck_string_free);
  require(value && length > 1 && length <= 128 * 1024 * 1024 + 1 && value[length - 1] == 0,
          "workspace ABI JSON ownership or length differs");
  return Json::parse(value, value + length - 1);
}
Json wait(Session &session, std::uint32_t timeout = 0) {
  return output([&](char **value, std::size_t *length) {
    return syrnck_workspace_wait_json(session.value, timeout, value, length);
  });
}
Json status(Session &session, OpenStore &store, const char *selector = nullptr, std::uint32_t mode = 0) {
  return output([&](char **value, std::size_t *length) {
    return syrnck_workspace_status_json(session.value, store.value, selector, mode, value, length);
  });
}
void observed(Session &session, std::string_view wanted) {
  const auto until = std::chrono::steady_clock::now() + 3s;
  do {
    const auto batch = wait(session, 25);
    if (batch["polling"] == true) return;
    for (const auto &path : batch["paths"])
      if (path.get<std::string>() == wanted) return;
  } while (std::chrono::steady_clock::now() < until);
  throw Error("workspace ABI notification missing: " + std::string(wanted));
}
void equivalent(Json observed, Json full) {
  observed.erase("working_index");
  observed.erase("observation");
  observed["hashed_bytes"] = full["hashed_bytes"];
  require(observed == full, "workspace ABI status differs from complete production scan");
}
void scenario(const fs::path &project, bool encrypted, bool polling) {
  std::optional<Key> key;
  if (encrypted) { key.emplace(); key->fill(57); }
  const auto write = [&](std::string_view path, std::string_view contents) {
    const auto destination = project / path_from_utf8(path);
    fs::create_directories(destination.parent_path());
    atomic_write(destination, bytes(contents), true);
  };
  std::string revision;
  {
    Store store(project / ".syrnck", key, true);
    write("asset", "before");
    write("large", std::string(4 * 1024 * 1024, 'x'));
    revision = store.commit_directory(project, "main", "workspace ABI baseline");
  }
  Session session(project, polling);
  const auto backend = wait(session)["backend"].get<std::string>();
  {
    OpenStore store(project, key);
    const auto initial = status(session, store);
    require(initial["clean"] == true && initial["base"] == revision &&
                initial["observation"]["full_scan"] == true && initial["hashed_bytes"] == 4 * 1024 * 1024 + 6,
            "workspace ABI startup trusted an incomplete inventory");
  }
  // No Store is retained by Session: both a normal C++ writer and ABI readers
  // can reopen it while the native watcher remains armed.
  { Store available(project / ".syrnck", key); available.gc(true); }
  write("asset", "after!");
  observed(session, "asset");
  write("new", "new");
  observed(session, "new"); // Wait twice without querying: both hints must survive.
  Json changed;
  {
    OpenStore store(project, key);
    changed = status(session, store);
    require(changed["clean"] == false && changed["workspace_files"] == 3 && changed["changes"].size() == 2,
            "workspace ABI dropped an earlier native batch");
    if (backend != "polling")
      require(changed["observation"]["mode"] == "targeted" && changed["hashed_bytes"] == 9,
              "workspace ABI reread unrelated file contents");
    require(status(session, store, revision.c_str(), 1)["base"] == revision, "exact revision selection differs");
  }
  {
    Store store(project / ".syrnck", key);
    equivalent(changed, store.status_directory(project, revision));
    store.stage_directory(project, "main", {"asset"});
  }
  write("asset", "later");
  observed(session, "asset");
  {
    OpenStore store(project, key);
    const auto combined = status(session, store, nullptr, 2);
    require(combined["branch"] == "main" && combined["staged"]["clean"] == false &&
                combined["unstaged"]["clean"] == false, "combined workspace ABI staging differs");
    char *value = reinterpret_cast<char *>(1);
    std::size_t length = 123;
    require(syrnck_workspace_status_json(session.value, store.value, nullptr, 9, &value, &length) == 1 &&
                !value && !length, "workspace ABI failure did not reset output ownership");
    require(status(session, store)["observation"]["full_scan"] == true, "failed status kept inventory continuity");
    require(syrnck_workspace_status_json(session.value, store.value, nullptr, 1, &value, &length) == 1 &&
                !value && !length, "revision mode accepted a missing selector");
    require(status(session, store)["observation"]["full_scan"] == true, "selector failure did not force recovery");
  }
  success(syrnck_workspace_invalidate(session.value));
  {
    OpenStore store(project, key);
    require(status(session, store)["observation"]["full_scan"] == true, "explicit audit reused working hashes");
  }
  auto other_path = project;
  other_path += "-other-store";
  { Store other(other_path, {}, true); }
  {
    SyrnckStore *other = nullptr;
    const auto path = utf8(other_path);
    success(syrnck_store_open(path.c_str(), false, nullptr, 0, &other));
    struct Close { SyrnckStore *value; ~Close() { syrnck_store_close(value); } } close{other};
    char *value = nullptr;
    std::size_t length = 123;
    require(syrnck_workspace_status_json(session.value, other, nullptr, 0, &value, &length) == 1 && !value && !length,
            "workspace ABI accepted another Store");
  }
  // Errors before/after a native wait must never allow a partial output or a
  // retained inventory to look current on the next successful request.
  char *value = reinterpret_cast<char *>(1);
  std::size_t length = 123;
  require(syrnck_workspace_wait_json(session.value, 60001, &value, &length) == 1 && !value && !length,
          "workspace ABI accepted an out-of-range wait");
  {
    OpenStore store(project, key);
    require(status(session, store)["observation"]["full_scan"] == true, "failed wait did not invalidate continuity");
  }
  write(".syrnckignore", "[z-a]\n");
  observed(session, ".syrnckignore");
  {
    OpenStore store(project, key);
    require(syrnck_workspace_status_json(session.value, store.value, nullptr, 0, &value, &length) == 1 &&
                !value && !length, "failed scan returned a previous status as current");
  }
  fs::remove(project / ".syrnckignore");
  {
    OpenStore store(project, key);
    require(status(session, store)["observation"]["full_scan"] == true, "scan failure did not invalidate inventory");
    require(syrnck_workspace_status_json(session.value, store.value, "", 0, &value, &length) == 1 &&
                !value && !length, "empty branch selector was treated as selected HEAD");
    value = reinterpret_cast<char *>(1);
    require(syrnck_workspace_wait_json(session.value, 0, &value, nullptr) == 1 && !value,
            "missing output size left a partial allocation");
    require(status(session, store)["observation"]["full_scan"] == true, "output failure did not require recovery");
  }
  Session restarted(project, polling);
  {
    OpenStore store(project, key);
    const auto report = status(restarted, store);
    require(report["observation"]["full_scan"] == true && report["hashed_bytes"] == 4 * 1024 * 1024 + 8,
            "a new ABI session trusted another session's cached inventory");
  }
}
void coalesced_overflow(const fs::path &project) {
  { Store store(project / ".syrnck", {}, true); }
  Session session(project, false, 1);
  const auto backend = wait(session)["backend"].get<std::string>();
  if (backend == "polling") return;
  { OpenStore store(project, {}); status(session, store); }
  { std::ofstream file(project / "one"); file << "one"; }
  observed(session, "one");
  { std::ofstream file(project / "two"); file << "two"; }
  observed(session, "two");
  OpenStore store(project, {});
  const auto result = status(session, store);
  require(result["workspace_files"] == 2 && result["observation"]["full_scan"] == true,
          "coalesced batches exceeded the hint budget without full recovery");
}
void detached_selection(const fs::path &project) {
  std::string detached, main;
  {
    Fixture fixture(project);
    detached = fixture.revision(Json{{"asset", fixture.file("detached")}});
    main = fixture.revision(Json{{"asset", fixture.file("main")}}, Json::array({detached}));
    fixture.install(main);
    fixture.store.detach_checkout(project, detached);
  }
  Session session(project, true);
  OpenStore store(project, {});
  const auto selected = status(session, store);
  require(selected["base"] == detached && selected["clean"] == true,
          "workspace ABI default used main instead of detached HEAD");
  const auto combined = status(session, store, nullptr, 2);
  require(combined["branch"] == ":HEAD" && combined["unstaged"]["clean"] == true,
          "workspace ABI combined status lost detached selection");
  const auto explicit_main = status(session, store, "main");
  require(explicit_main["base"] == main && explicit_main["clean"] == false,
          "workspace ABI explicit branch override ignored");
}
} // namespace
int main() {
  Temporary temporary;
  try {
    for (bool encrypted : {false, true})
      for (bool polling : {false, true})
        scenario(temporary.path / (std::to_string(encrypted) + '-' + std::to_string(polling)), encrypted, polling);
    coalesced_overflow(temporary.path / "overflow");
    detached_selection(temporary.path / "detached");
    std::cout << "Workspace ABI retained hints, targeted/polling status, ownership and recovery passed\n";
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
