#include "native_history_fixture.hpp"
#include "native_context.hpp"
#include <syrnck/backup.hpp>
#include <syrnck/bisect.hpp>
#include <syrnck/working_index.hpp>
#include <syrnck/stash.hpp>
#include <syrnck/workspace.hpp>
#include <syrnck/watcher.hpp>
#include <iostream>
using namespace native_test;
namespace {
void scenario(const fs::path &base, std::optional<Key> key, unsigned physical) {
  const auto primary = base / "primary", edit = base / "edit", view = base / "view";
  Fixture f(primary, key);
  const auto a = f.revision(Json{{"asset", f.file("first")}, {"shape", f.file("flat")}});
  const auto b = f.revision(Json{{"asset", f.file("main")}, {"shape/child", f.file("nested")}}, Json::array({a}));
  f.install(b);
  f.store.create_native_branch("feature", a);
  f.write("asset", "primary staged");
  const auto primary_index = f.store.stage_directory(primary, "main", {"asset"});
  f.store.register_native_workspace(primary, "edit", edit);
  f.store.register_native_workspace(primary, "view", view);
  f.store.use_native_workspace("edit");
  const auto initialized = f.store.switch_directory(edit, "feature");
  require(initialized["version"] == 8 && initialized["context"] == "edit" &&
              f.store.native_head()["branch"] == "feature" && text(read_bytes(edit / "asset")) == "first",
          "registered workspace did not initialize independently");
  atomic_write(edit / "asset", bytes("edit staged"), true);
  const auto edit_index = f.store.stage_directory(edit, "feature", {"asset"});
  f.store.use_native_workspace("view");
  f.store.detach_checkout(view, a);
  atomic_write(view / "asset", bytes("view staged"), true);
  const auto view_index = f.store.stage_directory(view, f.store.native_target(), {"asset"});
  const auto guarded_refs = f.store.refs();
  bool callback_attempted = false;
  fails([&] { f.store.commit_staged(f.store.native_target(), "reentrant selection", {{}, [&](const ProgressEvent &event) {
    if (event.phase == ProgressPhase::publishing) {
      callback_attempted = true;
      f.store.use_native_workspace("edit");
    }
  }}); });
  require(callback_attempted && f.store.native_workspace_name() == "view" && f.store.refs() == guarded_refs,
          "progress callback redirected context or published a revision");
  indexed_native_status(f.store, view, a);
  const auto cache = f.store.root() / "workspaces/view/working-index.json";
  const auto cache_bytes = read_bytes(cache);
  fails([&] { clear_native_working_index(f.store, {{}, [&](const ProgressEvent &) {
    f.store.use_native_workspace("");
  }}); });
  require(f.store.native_workspace_name() == "view" && read_bytes(cache) == cache_bytes,
          "progress callback redirected a cache clear");
  f.store.use_native_workspace("edit");
  f.store.use_native_workspace("view");
  f.pack(physical);
  const auto refs = f.store.refs();
  require(refs.at("staging/main").id == primary_index &&
              refs.at("workspaces/edit/staging/" + hash(bytes("feature"))).id == edit_index &&
              refs.at("workspaces/view/workspace/staging").id == view_index,
          "workspace indexes are not independent retained roots");
  const auto viewed = f.store.commit_staged(f.store.native_target(), "view child");
  f.store.use_native_workspace("edit");
  const auto edited = f.store.commit_staged("feature", "edit child");
  require(f.store.metadata(Kind::revision, edited)["parents"] == Json::array({a}), "linked branch parent changed");
  const auto before = f.physical();
  const auto blocked = f.store.plan_switch(edit, "main");
  require(blocked["ready"] == false && f.physical() == before,
          "branch reserved by primary was not blocked by read-only preflight");
  fails([&] { f.store.switch_directory(edit, "main"); });
  fails([&] { f.store.commit_directory(edit, "main", "foreign branch"); });
  fails([&] { f.store.status_directory(view, viewed); });
  fails([&] { f.store.set_ref("heads/main", {Kind::revision, edited}); });
  require(f.physical() == before && f.store.checkout_status()["pending"] == false,
          "blocked workspace/branch operation wrote data or began a journal");
  atomic_write(edit / "asset", bytes("local edit"), true);
  NativeStash stash(f.store);
  const auto saved = stash.save(edit, "feature", "linked work");
  require(text(read_bytes(edit / "asset")) == "edit staged", "linked stash did not clean its own working files");
  const auto id = saved["stash"]["id"].get<std::string>();
  const auto plan = stash.plan_apply(edit, "feature", id);
  stash.apply(edit, "feature", id, plan["plan"].get<std::string>(), true);
  require(text(read_bytes(edit / "asset")) == "local edit" && text(read_bytes(view / "asset")) == "view staged" &&
              text(read_bytes(primary / "asset")) == "primary staged", "linked stash touched another workspace");
  indexed_native_status(f.store, edit, edited);
  require(fs::is_regular_file(f.store.root() / "workspaces/edit/working-index.json"), "linked working index is not scoped");
  f.store.use_native_workspace("view");
  indexed_native_status(f.store, view, viewed);
  require(fs::is_regular_file(f.store.root() / "workspaces/view/working-index.json"), "second working index overwrote first");
  WorkspaceWatchOptions watch_options; watch_options.external_store = true;
  WorkspaceWatcher watcher(view, f.store.root(), watch_options);
  f.store.set_ref("pins/observer", {Kind::revision, viewed});
  const auto changes = watcher.wait(std::chrono::milliseconds(2000));
  require(changes.full_scan || changes.polling, "linked watcher missed external Store metadata publication");
  for (const auto &path : changes.paths)
    require(!path.starts_with(".."), "linked watcher leaked an external path as a workspace hint");
  f.store.use_native_workspace("");
  require(f.store.staged_status("main")["snapshot"] == primary_index && f.store.native_head()["revision"] == b,
          "linked operations changed primary HEAD/index");
  const auto main = f.store.commit_staged("main", "primary child");
  const auto listing = f.store.native_workspaces();
  require(listing["workspaces"].size() == 3 && listing["selected"] == "primary", "workspace inventory differs");
  const auto primary_search = NativeBisect(f.store).start(a, main);
  f.store.use_native_workspace("edit");
  require(NativeBisect(f.store).status()["active"] == false, "linked bisect inherited a primary search");
  const auto edit_search = NativeBisect(f.store).start(a, edited);
  f.store.use_native_workspace("view");
  const auto view_search = NativeBisect(f.store).start(a, viewed);
  NativeBisect(f.store).reset(view_search["state"].get<std::string>());
  require(NativeBisect(f.store).status()["active"] == false, "linked bisect did not reset its own state");
  f.store.use_native_workspace("edit");
  require(NativeBisect(f.store).status()["state"] == edit_search["state"], "another context reset a linked bisect");
  f.store.use_native_workspace("");
  require(NativeBisect(f.store).status()["state"] == primary_search["state"], "linked searches changed primary bisect state");
  fails([&] { f.store.register_native_workspace(primary, "nested", primary / "nested"); });
  fails([&] { f.store.register_native_workspace(primary, "nested", edit / "nested"); });
  fails([&] { f.store.register_native_workspace(primary, "edit", base / "duplicate"); });
  f.store.gc(true);
  f.store.export_revision(viewed, f.store.root() / "retained-view");
  require(text(read_bytes(f.store.root() / "retained-view/asset")) == "view staged", "GC lost linked detached bytes");
  const auto backup_path = base / "backup";
  f.store.use_native_workspace("edit");
  const auto frozen = f.store.stage_directory(edit, "feature", {"asset"});
  f.store.use_native_workspace("");
  create_backup(f.store, backup_path, key);
  Store backup(backup_path, key);
  const auto backup_refs = backup.refs();
  require(!backup_refs.contains("workspaces/edit/registration") && !backup_refs.contains("workspaces/edit/workspace/HEAD") &&
              backup_refs.at("workspaces/edit/snapshot").id == edited &&
              backup_refs.at("workspaces/view/snapshot").id == viewed && backup_refs.at("heads/main").id == main,
          "backup retained machine bindings or lost exact context revisions");
  const auto portable = parse_json(backup.get(backup_refs.at("workspaces/edit/portable")));
  require(portable["branch"] == "feature" && portable["revision"] == edited && !portable.contains("directory"),
          "portable workspace selection lost branch/revision or kept an absolute path");
  verify_backup(backup);
  const auto recovered = base / "recovered";
  fs::create_directory(recovered);
  restore_backup(backup, recovered / ".syrnck", key);
  Store restored(recovered / ".syrnck", key);
  NativeWorkspaceRestoreOptions restore_settings;
  restore_settings.detached = true;
  const auto destination = base / "restored-edit";
  const auto restored_context = restored.restore_native_workspace(recovered, "edit", destination, restore_settings);
  restored.use_native_workspace("edit");
  require(restored.native_head()["revision"] == edited && restored.native_head()["branch"].is_null() &&
              text(read_bytes(destination / "asset")) == "local edit" &&
              restored.staged_status(restored.native_target())["snapshot"] == restored_context["index"] &&
              restored_context["index"] != frozen,
          "portable context restore lost its frozen index or selected another revision");
  require(restored.metadata(Kind::revision, restored_context["index"].get<std::string>())["message"] == "Syrnck staging v1: :HEAD" &&
              restored.refs().at("archive/reset-index/" + frozen).id == frozen,
          "restored detached index did not retain the original named snapshot");
  restored.gc(true);
  restored.fsck();
  if (physical == 0) {
    const auto file = f.store.metadata(Kind::revision, viewed)["files"]["asset"]["manifest"].get<std::string>();
    const auto chunk = f.store.metadata(Kind::manifest, file)["blocks"][0]["id"].get<std::string>();
    const auto missing = f.store.root() / "objects/chunk" / chunk.substr(0, 2) / chunk;
    require(fs::remove(missing), "linked repair fixture did not remove its loose block");
    f.store.clear_cache();
    const auto repair_root = base / "repaired";
    fs::create_directory(repair_root);
    const auto repaired = create_repaired_copy(f.store, backup, repair_root / ".syrnck", key);
    require(repaired["recovered_objects"] == 1 && !fs::exists(missing), "linked repair copy changed source evidence or lost the missing block");
    Store repair(repair_root / ".syrnck", key);
    repair.restore_native_workspace(repair_root, "view", base / "repaired-view");
    require(text(read_bytes(base / "repaired-view/asset")) == "view staged", "repair copy lost portable workspace selection or data");
    repair.fsck();
  }
}
void recovery(const fs::path &base, bool abort) {
  const auto primary = base / "primary", edit = base / "edit";
  std::string a, b;
  {
    Fixture f(primary);
    a = f.revision(Json{{"asset", f.file("first")}, {"shape", f.file("flat")}});
    b = f.revision(Json{{"asset", f.file("main")}, {"shape/child", f.file("nested")}}, Json::array({a}));
    f.install(b);
    f.store.register_native_workspace(primary, "edit", edit);
    f.store.use_native_workspace("edit");
    f.store.detach_checkout(edit, b);
    std::stop_source stop;
    fails([&] { f.store.detach_checkout(edit, a, {stop.get_token(), [&](const ProgressEvent &event) {
      if (event.phase == ProgressPhase::publishing && event.path == "checkout") stop.request_stop();
    }}); });
    require(f.store.checkout_status()["pending"] == true, "linked interruption left no recoverable journal");
  }
  Store store(primary / ".syrnck");
  require(store.checkout_status()["pending"] == false, "primary read a linked workspace's pending record");
  fails([&] { store.gc(true); });
  store.use_native_workspace("edit");
  require(store.checkout_status()["context"] == "edit", "fresh handle selected wrong recovery context");
  store.recover_checkout(abort);
  require(store.native_head()["revision"] == (abort ? b : a) &&
              text(read_bytes(edit / "asset")) == (abort ? "main" : "first") &&
              text(read_bytes(primary / "asset")) == "main", "linked recovery changed another workspace or chose wrong target");
  store.gc(true);
  store.fsck();
}
void restore_invalid_index(const fs::path &base) {
  Fixture f(base / "primary");
  const auto revision = f.revision(Json{{"asset", f.file("committed")}});
  f.install(revision);
  f.store.register_native_workspace(f.project, "edit", base / "edit");
  f.store.use_native_workspace("edit");
  f.store.detach_checkout(base / "edit", revision);
  atomic_write(base / "edit/asset", bytes("frozen"), true);
  f.store.stage_directory(base / "edit", f.store.native_target(), {"asset"});
  create_backup(f.store, base / "backup");
  Store backup(base / "backup");
  fs::create_directory(base / "recovered");
  restore_backup(backup, base / "recovered/.syrnck");
  Store restored(base / "recovered/.syrnck");
  const auto chunk = restored.refs().at("workspaces/edit/portable");
  for (const auto &invalid : {ObjectKey{Kind::revision, revision}, chunk}) {
    restored.set_ref("workspaces/edit/workspace/staging", invalid);
    const auto before = restored.refs();
    fails([&] { restored.restore_native_workspace(base / "recovered", "edit", base / "destination"); });
    require(restored.refs() == before && !fs::exists(base / "destination"),
            "restoration bound or exported an invalid unchanged-target index");
  }
}
void restore_retry(const fs::path &base) {
  const auto primary = base / "primary", edit = base / "edit", recovered = base / "recovered", destination = base / "restored";
  std::string revision;
  {
    Fixture f(primary);
    revision = f.revision(Json{{"asset", f.file("committed")}});
    f.install(revision);
    f.store.register_native_workspace(primary, "edit", edit);
    f.store.use_native_workspace("edit");
    f.store.detach_checkout(edit, revision);
    atomic_write(edit / "asset", bytes("frozen bytes"), true);
    f.store.stage_directory(edit, f.store.native_target(), {"asset"});
    create_backup(f.store, base / "backup");
    Store backup(base / "backup");
    fs::create_directory(recovered);
    restore_backup(backup, recovered / ".syrnck");
  }
  {
    Store store(recovered / ".syrnck");
    const auto refs = store.refs();
    std::stop_source stop;
    NativeWorkspaceRestoreOptions settings; settings.branch = "rescue";
    fails([&] { store.restore_native_workspace(recovered, "edit", destination, settings,
        {stop.get_token(), [&](const ProgressEvent &event) {
          if (event.phase == ProgressPhase::publishing && event.path == "workspace-restore") stop.request_stop();
        }}); });
    require(store.refs() == refs && text(read_bytes(destination / "asset")) == "frozen bytes",
            "interrupted workspace restore published partial bindings or lost exported prepared data");
  }
  Store store(recovered / ".syrnck");
  const auto before = store.refs();
  NativeWorkspaceRestoreOptions settings; settings.branch = "rescue";
  atomic_write(destination / "asset", bytes("user edit after interruption"), true);
  fails([&] { store.restore_native_workspace(recovered, "edit", destination, settings); });
  require(store.refs() == before && text(read_bytes(destination / "asset")) == "user edit after interruption",
          "workspace restore overwrote a changed export on retry");
  atomic_write(destination / "asset", bytes("frozen bytes"), true);
  const auto restored = store.restore_native_workspace(recovered, "edit", destination, settings);
  require(restored["reused_directory"] == true && restored["branch"] == "rescue", "workspace restore could not resume its complete export");
  store.use_native_workspace("edit");
  require(store.native_head()["revision"] == revision && store.native_head()["branch"] == "rescue" &&
              store.staged_status("rescue")["snapshot"] == restored["index"] &&
              store.workspace_status(destination, "rescue")["unstaged"]["clean"] == true,
          "workspace restore did not preserve selected head and prepared working data");
  store.gc(true);
  store.fsck();
}
} // namespace
int main() {
  Temporary temporary;
  try {
    for (bool encrypted : {false, true})
      for (unsigned physical = 0; physical < 3; ++physical) {
        std::optional<Key> key;
        if (encrypted) { key.emplace(); key->fill(34); }
        scenario(temporary.path / (std::to_string(encrypted) + '-' + std::to_string(physical)), key, physical);
      }
    for (bool abort : {false, true}) recovery(temporary.path / (abort ? "abort" : "resume"), abort);
    restore_retry(temporary.path / "restore-retry");
    restore_invalid_index(temporary.path / "restore-invalid-index");
    std::cout << "Linked workspace selection/indexes, branch reservations, recovery, GC and portable backups\n";
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
