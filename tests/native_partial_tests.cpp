#include "native_history_fixture.hpp"
#include "native_materialization.hpp"
#include <syrnck/backup.hpp>
#include <syrnck/working_index.hpp>
#include <syrnck/workspace.hpp>
#include <syrnck/stash.hpp>
#include <syrnck/checkout_retention.hpp>
#include <syrnck/watcher.hpp>
#include <iostream>
using namespace native_test;
namespace {
void changed_policy_during_plan(const fs::path &root, bool reset = false) {
  Fixture f(root);
  const auto code = f.file("code\n");
  const auto a = f.revision(Json{{"Content", f.file("flat asset")}, {"Source/code", code}});
  const auto b = f.revision(Json{{"Content/asset", f.file("nested asset")}, {"Source/code", code}}, Json::array({a}));
  f.install(a);
  const auto policy = f.store.put(Kind::chunk, bytes(NativeMaterialization::from_prefixes({"Source"}).encode())).first;
  const auto reset_plan = reset ? plan_native_hard_reset(f.store, root, "main", b)["plan"].get<std::string>() : "";
  bool changed = false;
  fails([&] {
    const OperationOptions options{{}, [&](const ProgressEvent &event) {
      if (!changed && event.phase == ProgressPhase::planning && event.path == b) {
        changed = true;
        // Low-level callers may temporarily release an unbound primary line.
        // Reinstall its same tip with a new mask while the old plan is in flight.
        f.store.remove_ref("heads/main");
        f.store.set_ref("workspace/materialization", policy);
        f.store.set_ref("heads/main", {Kind::revision, a});
      }
    }};
    if (reset) native_hard_reset(f.store, root, "main", a, b, reset_plan, options);
    else f.store.checkout_directory(root, "main", b, options);
  });
  require(changed && f.store.refs().at("heads/main").id == a &&
              f.store.checkout_status()["pending"] == false &&
              text(read_bytes(root / "Content")) == "flat asset" &&
              text(read_bytes(root / "Source/code")) == "code\n",
          "changed planning policy began an unrecoverable checkout or changed working files");
}
void scenario(const fs::path &base, std::optional<Key> key, unsigned physical) {
  const auto primary = base / "primary", edit = base / "edit";
  Fixture f(primary, key);
  const auto asset = f.file(std::string(8192, 'A'));
  const auto other_asset = f.file(std::string(8192, 'B'));
  const auto ignore = f.file("*.tmp\n");
  const auto source_ignore = f.file("# Source rules\n");
  const auto a = f.revision(Json{{".syrnckignore", ignore}, {"Content/asset", asset},
      {"Source/.syrnckignore", source_ignore}, {"Source/code.cpp", f.file("first\n")},
      {"Source/shape", f.file("flat")}});
  const auto b = f.revision(Json{{".syrnckignore", ignore}, {"Content/asset", other_asset},
      {"Source/.syrnckignore", source_ignore}, {"Source/Code.cpp", f.file("second\n")},
      {"Source/shape/child", f.file("nested")}}, Json::array({a}));
  f.install(a);
  f.store.create_native_branch("partial", a);
  f.store.register_partial_native_workspace(primary, "edit", edit, {"Source"});
  f.store.use_native_workspace("edit");
  const auto created = f.store.switch_directory(edit, "partial");
  require(created["version"] == 9 && created["source_materialization"] == Json::array({"Source"}) &&
              created["target_materialization"] == Json::array({"Source"}) && created["context"] == "edit",
          "partial checkout did not freeze its physical projection");
  require(fs::is_regular_file(edit / ".syrnckignore") && fs::is_regular_file(edit / "Source/.syrnckignore") &&
              !fs::exists(edit / "Content") && text(read_bytes(edit / "Source/code.cpp")) == "first\n",
          "partial checkout omitted controls or materialized excluded assets");
  require(f.store.status_directory(edit, a)["clean"] == true && indexed_native_status(f.store, edit, a)["clean"] == true,
          "unmaterialized files were reported as physical deletions");
  WorkspaceObservation observation(edit, f.store.root());
  require(observation.status(f.store, a, {})["clean"] == true, "initial partial observation differs");
  const auto mask = f.store.refs().at("workspaces/edit/workspace/materialization");
  require(f.store.required_features() == std::vector<std::string>{"linked-workspaces-v1", "sparse-workspaces-v1"},
          "partial registration did not establish both reader requirements");
  const auto before_invalid = f.store.refs();
  const auto invalid_policy = f.store.put(Kind::chunk, bytes("null")).first;
  fails([&] { f.store.set_ref("workspaces/invalid/workspace/materialization", invalid_policy); });
  require(f.store.refs() == before_invalid, "invalid new policy changed retained roots");
  fails([&] { f.store.commit_directory(edit, "unborn-partial", "no logical baseline"); });
  fails([&] { f.store.stage_directory(edit, "unborn-partial", {"."}); });
  require(!f.store.refs().contains("heads/unborn-partial"), "partial import created an incomplete logical branch");
  const auto changed = f.store.put(Kind::chunk, bytes(NativeMaterialization::from_prefixes({"Content"}).encode())).first;
  fails([&] { f.store.set_ref("workspaces/edit/workspace/materialization", changed); });
  fails([&] { f.store.compare_update_refs({{"workspaces/edit/workspace/materialization", mask, {}}}); });
  require(f.store.refs().at("workspaces/edit/workspace/materialization") == mask,
          "live projection was changed without a filesystem transition");
  fs::create_directories(edit / "Content");
  atomic_write(edit / "Content/local", bytes("keep outside selection"), false);
  WorkspaceChanges outside;
  outside.paths = {"Content/local"};
  const auto outside_status = observation.status(f.store, a, outside);
  require(outside_status["clean"] == true && outside_status["observation"]["mode"] == "targeted" &&
              outside_status["observation"]["inspected_files"] == 0,
          "outside-mask hint polluted the selected observation");
  atomic_write(edit / "Source/code.cpp", bytes("staged\n"), true);
  atomic_write(edit / "Source/ignored.tmp", bytes("ignored"), false);
  const auto staged = f.store.stage_directory(edit, "partial", {"."});
  require(f.store.metadata(Kind::revision, staged)["files"]["Content/asset"] == asset &&
              !f.store.metadata(Kind::revision, staged)["files"].contains("Source/ignored.tmp"),
          "stage discarded hidden logical entries or ignored controls");
  fails([&] { f.store.stage_directory(edit, "partial", {"Content/asset"}); });
  fails([&] { f.store.diff_working_file(edit, staged, "Content/asset"); });
  const auto committed = f.store.commit_staged("partial", "partial staged commit");
  require(f.store.metadata(Kind::revision, committed)["files"]["Content/asset"] == asset,
          "staged commit lost omitted asset bytes");
  atomic_write(edit / "Source/code.cpp", bytes("amended\n"), true);
  const auto amended = f.store.amend_directory(edit, "partial", committed, "partial amendment");
  require(f.store.metadata(Kind::revision, amended)["files"]["Content/asset"] == asset,
          "directory amendment lost omitted asset bytes");
  atomic_write(edit / "Source/code.cpp", bytes("directory\n"), true);
  const auto direct = f.store.commit_directory(edit, "partial", "partial directory commit");
  require(f.store.metadata(Kind::revision, direct)["files"]["Content/asset"] == asset &&
              f.store.metadata(Kind::revision, direct)["parents"] == Json::array({amended}),
          "directory commit changed hidden entries or parent selection");
  const auto reset_plan = plan_native_hard_reset(f.store, edit, "partial", b, true);
  require(reset_plan["version"] == 2 && reset_plan["ready"] == true &&
              reset_plan["materialization"] == Json::array({"Source"}) &&
              !reset_plan["working"].contains("Content/local"),
          "partial hard reset did not plan the selected physical tree");
  NativeStash stash(f.store);
  fails([&] { stash.save(edit, "partial", "unsupported partial stash"); });
  const auto transitioned = f.store.checkout_directory(edit, "partial", b);
  require(transitioned["version"] == 9 && transitioned["layout_version"] == 3 &&
              text(read_bytes(edit / "Source/shape/child")) == "nested" &&
              text(read_bytes(edit / "Source/Code.cpp")) == "second\n" &&
              text(read_bytes(edit / "Content/local")) == "keep outside selection" && !fs::exists(edit / "Content/asset"),
          "partial namespace transition changed excluded physical data");
  require(f.store.status_directory(edit, b)["clean"] == true, "case transition changed partial status");
  f.pack(physical);
  f.store.gc(true);
  const auto exported = base / "complete-export";
  f.store.export_revision(b, exported);
  require(text(read_bytes(exported / "Content/asset")) == std::string(8192, 'B'),
          "GC/export could not reconstruct hidden asset blocks");
  const auto receipts = native_checkout_receipts(f.store, {}, 100, true);
  require(!receipts.at("transactions").empty(), "verified partial receipts were lost after GC");
  f.store.use_native_workspace("");
  const auto asset_directory = base / "asset-view";
  f.store.register_partial_native_workspace(primary, "asset-view", asset_directory, {"Content/asset"});
  f.store.use_native_workspace("asset-view");
  f.store.detach_checkout(asset_directory, a);
  require(!fs::exists(asset_directory / "Source") && fs::is_regular_file(asset_directory / ".syrnckignore") &&
              text(read_bytes(asset_directory / "Content/asset")) == std::string(8192, 'A'),
          "literal file projection did not select its ancestor controls");
  f.store.detach_checkout(asset_directory, b);
  require(text(read_bytes(asset_directory / "Content/asset")) == std::string(8192, 'B') &&
              f.store.status_directory(asset_directory, b)["clean"] == true,
          "literal file projection did not replace selected blocks");
  f.store.use_native_workspace("");
  const auto empty = base / "empty";
  f.store.register_partial_native_workspace(primary, "empty", empty, {});
  f.store.use_native_workspace("empty");
  f.store.detach_checkout(empty, b);
  require(fs::is_empty(empty) && f.store.status_directory(empty, b)["clean"] == true,
          "explicit empty projection was treated as complete");
  const auto empty_commit = f.store.commit_directory(empty, f.store.native_target(), "metadata only view");
  require(f.store.metadata(Kind::revision, empty_commit)["files"] == f.store.metadata(Kind::revision, b)["files"],
          "empty projection commit deleted its complete tree");
  atomic_write(empty / ".syrnckignore", bytes("[invalid regex control outside selection"), false);
  require(f.store.status_directory(empty, empty_commit)["clean"] == true,
          "empty materialization read unselected ignore rules");
  f.store.use_native_workspace("edit");
  atomic_write(edit / "Source/Code.cpp", bytes("frozen index\n"), true);
  const auto frozen = f.store.stage_directory(edit, "partial", {"."});
  f.store.use_native_workspace("");
  const auto backup_path = base / "backup";
  create_backup(f.store, backup_path, key);
  Store backup(backup_path, key);
  verify_backup(backup);
  require(backup.refs().at("workspaces/edit/workspace/materialization") == mask,
          "portable backup lost the materialization root");
  const auto recovered = base / "recovered";
  fs::create_directory(recovered);
  restore_backup(backup, recovered / ".syrnck", key);
  Store restored(recovered / ".syrnck", key);
  const auto destination = base / "restored-edit";
  NativeWorkspaceRestoreOptions settings;
  settings.detached = true;
  const auto report = restored.restore_native_workspace(recovered, "edit", destination, settings);
  restored.use_native_workspace("edit");
  require(restored.native_head()["revision"] == b && !fs::exists(destination / "Content") &&
              text(read_bytes(destination / "Source/Code.cpp")) == "frozen index\n" &&
              restored.metadata(Kind::revision, report["index"].get<std::string>())["files"]["Content/asset"] == other_asset &&
              restored.metadata(Kind::revision, frozen)["files"]["Content/asset"] == other_asset,
          "restored partial context lost its complete index or projected worktree");
  restored.gc(true);
  restored.fsck();
  restored.export_revision(b, base / "restored-complete");
  require(text(read_bytes(base / "restored-complete/Content/asset")) == std::string(8192, 'B'),
          "portable partial restore discarded hidden asset versions");
  std::cout << "Partial workspaces preserve full logical trees, namespace recovery and portable indexes (pack="
            << physical << ", encrypted=" << key.has_value() << ")\n";
}
}
int main() {
  try {
    Temporary temporary;
    changed_policy_during_plan(temporary.path / "changed-policy");
    changed_policy_during_plan(temporary.path / "reset-changed-policy", true);
    for (unsigned physical = 0; physical < 3; ++physical)
      for (bool encrypted : {false, true}) {
        const auto root = temporary.path / (std::to_string(physical) + (encrypted ? "-encrypted" : "-plain"));
        fs::create_directories(root);
        std::optional<Key> key;
        if (encrypted) { key = Key{}; key->fill(73); }
        scenario(root, key, physical);
      }
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
