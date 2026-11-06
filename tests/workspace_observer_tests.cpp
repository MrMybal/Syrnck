#include <syrnck/watcher.hpp>
#include "internal.hpp"
#include <iostream>
using namespace syrnck;
namespace {
void write(const fs::path &path, std::string_view value) {
  fs::create_directories(path.parent_path());
  atomic_write(path, bytes(value), true);
}
void equivalent(Json observed, Json full) {
  observed.erase("working_index");
  observed.erase("observation");
  observed["hashed_bytes"] = full["hashed_bytes"];
  require(observed == full, "session observation differs from complete status");
}
void scenario(const fs::path &root, bool encrypted) {
  std::optional<Key> key;
  if (encrypted) { key.emplace(); key->fill(53); }
  const auto project = root / "project", storage = project / "vault";
  std::string revision;
  const auto large = std::string(4 * 1024 * 1024, 'x');
  {
    Store store(storage, key, true);
    write(project / "asset", "before");
    write(project / "unrelated/large", large);
    write(project / "nested/code.cpp", "return 0;\n");
    revision = store.commit_directory(project, "main", "baseline");
  }
  WorkspaceObservation session(project, storage);
  const auto check = [&](const WorkspaceChanges &hints, std::string_view mode) {
    Store store(storage, key);
    const auto observed = session.status(store, revision, hints);
    equivalent(observed, store.status_directory(project, revision));
    require(observed["observation"]["mode"].get<std::string>() == mode, "unexpected workspace observation mode");
    return observed;
  };
  const auto initial = check({}, "full");
  require(initial["hashed_bytes"].get<std::uint64_t>() >= large.size(), "initial observation skipped contents");
  const auto timestamp = fs::last_write_time(project / "asset");
  write(project / "asset", "after!");
  fs::last_write_time(project / "asset", timestamp);
  const auto changed = check({{"asset", "asset"}}, "targeted");
  require(changed["hashed_bytes"] == 6 && changed["observation"]["inspected_files"] == 1 &&
              changed["observation"]["retained_files"] == 2, "targeted edit reread unrelated contents");
  // Complete inventory persists across Store lifetimes and plain/encrypted
  // cache GC, rather than depending on the subset of persisted stat entries.
  {
    Store store(storage, key);
    store.gc(true);
  }
  write(project / "nested/code.cpp", "return 1;\n");
  bool unrelated_scanned = false;
  {
    Store store(storage, key);
    const auto observed = session.status(store, revision, {{"nested/code.cpp"}}, {{}, [&](const ProgressEvent &e) {
      if (e.phase == ProgressPhase::scanning && e.path.starts_with("unrelated")) unrelated_scanned = true;
    }});
    equivalent(observed, store.status_directory(project, revision));
    require(observed["hashed_bytes"] == 10 && !unrelated_scanned,
            "targeted edit traversed unrelated subtree");
  }
  write(project / "new", "added");
  check({{"new"}}, "full");
  fs::remove(project / "new");
  check({{"new"}}, "full");
  fs::rename(project / "nested", project / "renamed");
  check({{"nested", "renamed"}}, "full");
  write(project / ".syrnckignore", "unrelated/\n");
  require(check({{".syrnckignore"}}, "full")["workspace_files"] == 3, "ignore change missed subtree");
  fs::remove(project / ".syrnckignore");
  check({{".syrnckignore"}}, "full");
  fs::remove(project / "asset");
  fs::create_directory(project / "asset");
  write(project / "asset/child", "child");
  check({{"asset"}}, "full");
  check({{}, false, false, true}, "indexed");
  const auto overflow = check({{}, true, true}, "full");
  require(overflow["working_index"]["reused_files"] == 0, "overflow retained advisory hashes");
  session.invalidate();
  check({{"renamed/code.cpp"}}, "full");
  {
    Store store(storage, key);
    std::stop_source stop;
    bool cancelled = false;
    try {
      session.status(store, revision, {{"renamed/code.cpp"}}, {stop.get_token(), [&](const ProgressEvent &e) {
        if (e.phase == ProgressPhase::verifying) stop.request_stop();
      }});
    } catch (const Cancelled &) { cancelled = true; }
    require(cancelled, "observation ignored cancellation");
  }
  check({{"renamed/code.cpp"}}, "full");
  {
    Store store(storage, key);
    store.stage_directory(project, "main", {"renamed/code.cpp"});
    write(project / "renamed/code.cpp", "later edit\n");
    auto observed = session.workspace_status(store, "main", {{"renamed/code.cpp"}});
    auto full = store.workspace_status(project, "main");
    equivalent(observed["unstaged"], full["unstaged"]);
    require(observed["staged"] == full["staged"] && observed["clean"] == full["clean"],
            "session staging status differs from complete status");
  }
  // A new session always rebuilds, even if another process populated the cache.
  WorkspaceObservation restarted(project, storage);
  Store store(storage, key);
  const auto report = restarted.status(store, revision, {{"renamed/code.cpp"}});
  require(report["observation"]["full_scan"] == true && report["working_index"]["reused_files"] == 0,
          "new session trusted persisted cache as a complete inventory");
}
} // namespace
int main() {
  const auto root = fs::temp_directory_path() / ("syrnck-workspace-observer-" + hex(random_bytes(16)));
  struct Cleanup { fs::path path; ~Cleanup() { std::error_code error; fs::remove_all(path, error); } } cleanup{root};
  try {
    for (bool encrypted : {false, true}) scenario(root / std::to_string(encrypted), encrypted);
    std::cout << "session workspace observations: plain/encrypted scenarios passed\n";
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
