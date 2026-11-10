#include "native_history_fixture.hpp"
#include "test_cases.hpp"
#include "native_materialization.hpp"
#include <syrnck/workspace.hpp>
#include <syrnck/watcher.hpp>
#include <syrnck/working_index.hpp>
#include <syrnck/backup.hpp>
#include <syrnck/checkout_retention.hpp>
#include <iostream>
using namespace native_test;
namespace {
std::function<void(std::string_view, const fs::path &)> hook;
using Selection = std::optional<std::vector<std::string>>;
Json change(Store &store, const fs::path &directory, const Selection &prefixes) {
  const auto before = store.refs();
  const auto plan = plan_native_materialization(store, directory, prefixes, true);
  require(plan["ready"] == true && store.refs() == before, "materialization plan is blocked or changed roots");
  require(plan_native_materialization(store, directory, prefixes)["plan"] == plan["plan"],
          "materialization plan ID changed with auditing");
  return set_native_materialization(store, directory, prefixes, plan["head"].get<std::string>(),
      plan["plan"].get<std::string>());
}
void workflow(const fs::path &base, std::optional<Key> key, unsigned physical, bool linked, bool detached) {
  Fixture f(base / "primary", key);
  const auto head = f.revision(Json{{".syrnckignore", f.file("*.tmp\n")},
      {"Source/code", f.file("code")}, {"Source/.syrnckignore", f.file("# source rules\n")},
      {"Content/A/leaf", f.file("selected asset")}, {"Content/B", f.file("hidden asset")}});
  f.install(head);
  auto directory = f.project;
  if (linked) {
    f.store.create_native_branch("feature", head);
    directory = base / "edit";
    f.store.register_native_workspace(f.project, "edit", directory);
    f.store.use_native_workspace("edit");
    f.store.switch_directory(directory, "feature");
  }
  if (detached) f.store.detach_checkout(directory, head);
  const auto selected = f.store.native_head();
  atomic_write(directory / "local.tmp", bytes("unrelated untracked"), false);
  WorkspaceObservation observation(directory, f.store.root());
  require(observation.status(f.store, head, {})["clean"] == true, "initial full observation is dirty");
  f.pack(physical);
  const auto a = change(f.store, directory, std::vector<std::string>{"Source"});
  require(a["version"] == 10 && a["layout_version"] == 7 && a["source_materialization"].is_null() &&
              a["target_materialization"] == Json::array({"Source"}) &&
              fs::is_regular_file(directory / "Source/code") && !fs::exists(directory / "Content/B") &&
              !fs::exists(directory / "Content/A/leaf") &&
              fs::is_regular_file(directory / ".syrnckignore") &&
              text(read_bytes(directory / "local.tmp")) == "unrelated untracked" && f.store.native_head() == selected,
          "full-to-partial transition lost files, controls, HEAD or unrelated work");
  require(observation.status(f.store, head, {})["clean"] == true &&
              indexed_native_status(f.store, directory, head)["clean"] == true,
          "observer/index did not adopt the newly committed projection");
  if (linked) {
    create_backup(f.store, base / "partial-backup", key);
    Store backup(base / "partial-backup", key);
    const auto primary = base / "restored-primary", view = base / "restored-edit";
    fs::create_directory(primary);
    restore_backup(backup, primary / ".syrnck", key);
    Store restored(primary / ".syrnck", key);
    NativeWorkspaceRestoreOptions settings;
    settings.detached = true;
    restored.restore_native_workspace(primary, "edit", view, settings);
    restored.use_native_workspace("edit");
    require(restored.native_materialization()["prefixes"] == Json::array({"Source"}) &&
                restored.required_features() == f.store.required_features() &&
                fs::is_regular_file(view / "Source/code") && !fs::exists(view / "Content/B"),
            "portable restore lost a changed selection or its reader requirements");
    restored.gc(true);
    restored.fsck();
    restored.export_revision(head, base / "restored-complete");
    require(text(read_bytes(base / "restored-complete/Content/B")) == "hidden asset",
            "portable selection restore lost excluded blocks after GC");
  }
  const auto root = f.store.refs().at(f.store.native_workspace_reference("workspace/materialization"));
  const auto invalid = f.store.put(Kind::chunk, bytes(NativeMaterialization::from_prefixes({"Content"}).encode())).first;
  fails([&] { f.store.set_ref(f.store.native_workspace_reference("workspace/materialization"), invalid); });
  require(f.store.refs().at(f.store.native_workspace_reference("workspace/materialization")) == root,
          "raw reference mutation bypassed the materialization transaction");
  change(f.store, directory, std::vector<std::string>{"Content/A/leaf"});
  require(!fs::exists(directory / "Source/code") && !fs::exists(directory / "Source/.syrnckignore") &&
              fs::is_regular_file(directory / "Content/A/leaf") &&
              !fs::exists(directory / "Content/B") && observation.status(f.store, head, {})["clean"] == true,
          "partial-to-partial transition did not replace its physical projection");
  change(f.store, directory, std::vector<std::string>{});
  require(!fs::exists(directory / ".syrnckignore") && !fs::exists(directory / "Content/A/leaf") &&
              observation.status(f.store, head, {})["clean"] == true,
          "empty projection left tracked files or used obsolete observation data");
  change(f.store, directory, {});
  require(fs::is_regular_file(directory / "Content/B") && fs::is_regular_file(directory / "Source/code") &&
              f.store.native_head() == selected && indexed_native_status(f.store, directory, head)["clean"] == true,
          "partial-to-full transition failed to reconstruct the complete tree");
  const auto stable = f.physical();
  require(change(f.store, directory, {})["state"] == "unchanged" && f.physical() == stable,
          "identical selection wrote data");
  f.store.gc(true);
  const auto receipts = native_checkout_receipts(f.store, {}, 100, true);
  require(receipts["transactions"].size() >= 4, "materialization receipts were not retained after GC");
  for (const auto &receipt : receipts["transactions"])
    (void)f.store.checkout_status(receipt["id"].get<std::string>());
  f.store.export_revision(head, base / "export");
  require(text(read_bytes(base / "export/Content/B")) == "hidden asset", "transition GC lost hidden blocks");
  create_backup(f.store, base / "backup", key);
  Store backup(base / "backup", key);
  verify_backup(backup);
  require(backup.required_features() == f.store.required_features(), "backup discarded transition reader requirements");
  f.store.fsck();
}
void refusals(const fs::path &base) {
  Fixture f(base);
  const auto head = f.revision(Json{{"Source/code", f.file("base code")}, {"Content/B", f.file("base asset")}});
  f.install(head);
  const Selection source = std::vector<std::string>{"Source"};
  const auto initial = plan_native_materialization(f.store, base, source);
  f.write("Source/code", "dirty code");
  auto before = f.physical();
  require(plan_native_materialization(f.store, base, source)["ready"] == false,
          "materialization shrink accepted dirty tracked files");
  fails([&] { set_native_materialization(f.store, base, source, head, initial["plan"].get<std::string>()); });
  require(f.physical() == before && f.store.required_features().empty(), "blocked materialization changed storage or capabilities");
  f.store.stage_directory(base, "main", {"Source"});
  before = f.physical();
  const auto blocked = plan_native_materialization(f.store, base, source);
  require(blocked["ready"] == false && !blocked["blockers"].empty(), "materialization accepted a frozen index");
  fails([&] { set_native_materialization(f.store, base, source, head, blocked["plan"].get<std::string>()); });
  require(change(f.store, base, {})["state"] == "unchanged" && f.physical() == before,
          "identical full selection altered dirty work or its frozen index");
  const auto reset = plan_native_hard_reset(f.store, base, "main", head);
  native_hard_reset(f.store, base, "main", head, head, reset["plan"].get<std::string>());
  change(f.store, base, source);
  f.write("Content/B", "excluded local blocker");
  before = f.physical();
  const auto grow = plan_native_materialization(f.store, base, {});
  require(grow["ready"] == false, "materialization grow adopted an excluded local file");
  fails([&] { set_native_materialization(f.store, base, {}, head, grow["plan"].get<std::string>()); });
  require(f.physical() == before && text(read_bytes(base / "Content/B")) == "excluded local blocker",
          "blocked materialization overwrote excluded work");
  fs::remove(base / "Content/B");
  const auto stale = plan_native_materialization(f.store, base, {});
  f.write("Source/code", "new committed code");
  const auto advanced = f.store.commit_directory(base, "main", "advance while planning");
  before = f.physical();
  fails([&] { set_native_materialization(f.store, base, {}, head, stale["plan"].get<std::string>()); });
  require(f.physical() == before && f.store.native_head()["revision"] == advanced,
          "stale materialization plan changed a newer head");
  fs::create_directory(base / "wrong");
  fails([&] { plan_native_materialization(f.store, base / "wrong", source); });
}
void recovery(const fs::path &base, std::string_view phase, bool abort, bool linked, bool detached) {
  Fixture f(base / "primary");
  const auto head = f.revision(Json{{"Source/code", f.file("code")}, {"Content/B", f.file("asset")}});
  f.install(head);
  auto directory = f.project;
  if (linked) {
    directory = base / "edit";
    f.store.create_native_branch("feature", head);
    f.store.register_native_workspace(f.project, "edit", directory);
    f.store.use_native_workspace("edit");
    f.store.switch_directory(directory, "feature");
  }
  if (detached) f.store.detach_checkout(directory, head);
  const auto selected = f.store.native_head();
  const Selection target = std::vector<std::string>{"Source"};
  const auto plan = plan_native_materialization(f.store, directory, target);
  bool paused = false;
  hook = [&](std::string_view event, const fs::path &) {
    if (!paused && event == phase) { paused = true; throw Error("controlled materialization pause"); }
  };
  fails([&] { set_native_materialization(f.store, directory, target, head, plan["plan"].get<std::string>()); });
  hook = {};
  require(paused && f.store.checkout_status()["pending"] == true, "materialization checkpoint was not reached");
  if (phase == "checkout.committed") fails([&] { f.store.recover_checkout(true); });
  f.store.recover_checkout(abort);
  require(f.store.checkout_status()["pending"] == false && f.store.native_head() == selected &&
              fs::is_regular_file(directory / "Source/code") &&
              fs::is_regular_file(directory / "Content/B") == abort &&
              f.store.native_materialization()["complete"] == abort,
          "materialization resume/abort split its physical files, policy or HEAD");
  f.store.gc(true);
  f.store.fsck();
}
}
namespace syrnck::testing {
void io_checkpoint(std::string_view phase, const fs::path &target) { if (hook) hook(phase, target); }
}
int main(int argc, char **argv) {
  Temporary temporary;
  try {
    fixture_test::TestCases cases(fixture_test::CaseSelection::parse(argc, argv));
    for (unsigned physical = 0; physical < 3; ++physical)
      for (bool encrypted : {false, true})
        for (bool linked : {false, true})
          for (bool detached : {false, true}) {
            std::optional<Key> key;
            if (encrypted) {
              key.emplace();
              key->fill(79);
            }
            cases.add("workflow-" + std::to_string(physical) + '-' + std::to_string(encrypted) + '-' +
                          std::to_string(linked) + '-' + std::to_string(detached),
                      [key, physical, linked, detached](const fs::path &path) {
                        workflow(path, key, physical, linked, detached);
                      });
          }
    cases.add("refusals", [](const fs::path &path) { refusals(path); });
    for (bool linked : {false, true})
      for (bool detached : {false, true})
        for (const auto phase : {"checkout.prepared", "checkout.applied", "checkout.committed"})
          for (bool abort : {false, true}) {
            if (abort && std::string_view(phase) == "checkout.committed")
              continue;
            cases.add("recovery-" + std::to_string(linked) + '-' + std::to_string(detached) + '-' +
                          std::string(phase).substr(9) + '-' + std::to_string(abort),
                      [phase, abort, linked, detached](const fs::path &path) {
                        recovery(path, phase, abort, linked, detached);
                      });
          }
    return cases.run(temporary.path);
  } catch (const std::exception &error) {
    hook = {};
    std::cerr << error.what() << '\n';
    return 1;
  }
}
