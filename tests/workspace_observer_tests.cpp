#include <syrnck/watcher.hpp>
#include "internal.hpp"
#include "native_workspace.hpp"
#include "native_inventory.hpp"
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
  require(check({{"new"}}, "targeted")["hashed_bytes"] == 5, "new file reread unrelated contents");
  fs::remove(project / "new");
  require(check({{"new"}}, "targeted")["hashed_bytes"] == 0, "deletion reread unrelated contents");
  fs::rename(project / "nested", project / "renamed");
  check({{"nested", "renamed"}}, "targeted");
  write(project / ".syrnckignore", "unrelated/\n");
  require(check({{".syrnckignore"}}, "full")["workspace_files"] == 3, "ignore change missed subtree");
  fs::remove(project / ".syrnckignore");
  check({{".syrnckignore"}}, "full");
  fs::remove(project / "asset");
  fs::create_directory(project / "asset");
  write(project / "asset/child", "child");
  check({{"asset"}}, "targeted");
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
void scoped_changes(const fs::path &root, bool encrypted) {
  std::optional<Key> key;
  if (encrypted) { key.emplace(); key->fill(61); }
  const auto project = root / "project";
  Store store(project / "vault", key, true);
  write(project / "unrelated/large", std::string(4 * 1024 * 1024, 'x'));
  write(project / ".syrnckignore", "*.tmp\nignored/\n");
  write(project / "edit/first", "one");
  write(project / "edit/second", "two");
  const auto baseline = store.commit_directory(project, "main", "scoped baseline");
  const auto refs = store.refs();
  WorkspaceObservation session(project, store.root());
  session.status(store, baseline, {});
  const auto query = [&](std::vector<std::string> paths, std::uint64_t expected_bytes) {
    bool unrelated = false;
    const auto report = session.status(store, baseline, {std::move(paths)}, {{}, [&](const ProgressEvent &e) {
      if (e.phase == ProgressPhase::scanning && e.path.starts_with("unrelated")) unrelated = true;
    }});
    equivalent(report, store.status_directory(project, baseline));
    require(report["observation"]["mode"] == "targeted" && !unrelated && report["hashed_bytes"] == expected_bytes,
            "scoped namespace update traversed or hashed unrelated files");
    require(store.refs() == refs, "scoped observation changed retained roots");
    return report;
  };
  write(project / "edit/new", "new");
  query({"edit/new"}, 3);
  fs::rename(project / "edit/new", project / "edit/renamed");
  query({"edit/renamed", "edit/new", "edit/renamed"}, 3);
  fs::remove(project / "edit/renamed");
  query({"edit/renamed"}, 0);
  write(project / "edit/cache.tmp", "ignored cache");
  query({"edit/cache.tmp"}, 0);
  write(project / "ignored/nested/file", "ignored subtree");
  require(query({"ignored/nested/file"}, 0)["clean"] == true, "ignored ancestor entered inventory");
  write(project / "editor/keep", "keep");
  query({"editor/keep"}, 4);
  fs::rename(project / "edit", project / "moved");
  const auto renamed = query({"edit/first", "moved/first", "moved", "moved/second"}, 6);
  require(renamed["observation"]["scanned_scopes"] == Json::array({"edit", "moved"}),
          "missing ancestor or overlapping hints produced incomplete scopes");
  fs::remove(project / "moved/first");
  fs::remove(project / "moved/second");
  fs::remove(project / "moved/cache.tmp");
  fs::remove(project / "moved");
  write(project / "moved", "plain");
  const auto file = query({"moved/first"}, 5);
  require(file["observation"]["scanned_scopes"] == Json::array({"moved"}),
          "regular-file ancestor did not replace the old directory scope");
  fs::remove(project / "moved");
  write(project / "moved/first", "one");
  write(project / "moved/second", "two");
  query({"moved/first", "moved"}, 6);
  write(project / "moved/.syrnckignore", "second\n");
  const auto ignored = query({"moved/.syrnckignore", "moved/first", "moved/second"}, 10);
  require(ignored["observation"]["scanned_scopes"] == Json::array({"moved"}),
          "nested ignore update did not coalesce its parent scope");
  fs::remove(project / "moved/.syrnckignore");
  query({"moved/.syrnckignore"}, 6);
  fs::remove(project / "moved/first");
  fs::remove(project / "moved/second");
  fs::remove(project / "moved");
  const auto deleted = query({"moved/first"}, 0);
  require(deleted["workspace_files"] == 3, "missing directory left stale sibling entries or removed prefix neighbor");
  // A special/unsafe namespace is never accepted by merely validating the
  // freshly enumerated scope; the whole retained+replacement union is checked.
  for (bool directory_collision : {false, true}) {
    NativeInventoryPaths names;
    names.add("Asset");
    bool refused = false;
    try {
      names.add(directory_collision ? "asset/child" : "asset");
      names.finish({});
    } catch (const Error &) { refused = true; }
    require(refused, "scoped namespace validation accepted a portable collision");
  }
  bool protected_refused = false;
  try { visit_native_scope(project, store, "vault", [](const NativeFile &) {}, {}); }
  catch (const Error &) { protected_refused = true; }
  require(protected_refused, "scoped scan entered protected Store contents");

  write(project / "directory/a", "one");
  write(project / "directory/b", "two");
  query({"directory"}, 6);
  fs::remove(project / "directory/a");
  std::stop_source stop;
  bool cancelled = false;
  try {
    session.status(store, baseline, {{"directory/a"}}, {stop.get_token(), [&](const ProgressEvent &e) {
      if (e.phase == ProgressPhase::scanning) stop.request_stop();
    }});
  } catch (const Cancelled &) { cancelled = true; }
  require(cancelled, "scope discovery ignored cancellation");
  const auto recovered = session.status(store, baseline, {{"directory/a"}});
  require(recovered["observation"]["full_scan"] == true, "cancelled scope query retained incremental continuity");
  equivalent(recovered, store.status_directory(project, baseline));
}
} // namespace
int main() {
  const auto root = fs::temp_directory_path() / ("syrnck-workspace-observer-" + hex(random_bytes(16)));
  struct Cleanup { fs::path path; ~Cleanup() { std::error_code error; fs::remove_all(path, error); } } cleanup{root};
  try {
    for (bool encrypted : {false, true}) {
      scenario(root / std::to_string(encrypted), encrypted);
      scoped_changes(root / ("scopes-" + std::to_string(encrypted)), encrypted);
    }
    std::cout << "session workspace observations: plain/encrypted scenarios passed\n";
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
