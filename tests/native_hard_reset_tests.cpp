#include "native_history_fixture.hpp"
#include "test_cases.hpp"
#include "native_target.hpp"
#include <syrnck/workspace.hpp>
#include <syrnck/backup.hpp>
#include <iostream>
using namespace native_test;
namespace {
std::function<void(std::string_view, const fs::path &)> hook;
struct Scenario {
  std::string target, head, index;
  explicit Scenario(Fixture &f) {
    target = f.revision(Json{{"a", f.file("target a")},
                             {"missing", f.file("target missing")},
                             {"restored", f.file("target restored")},
                             {"shape", f.file("target flat")}});
    head = f.revision(Json{{"a", f.file("head a")},
                           {"missing", f.file("head missing")},
                           {"shape/child", f.file("head child")},
                           {"remove", f.file("head remove")}},
                      Json::array({target}));
    f.install(head);
    f.write("a", "staged a");
    f.write("new", "staged new");
    index = f.store.stage_directory(f.project, "main", {"a", "new"});
    f.write("a", "working a");
    f.write("new", "working new");
    fs::remove(f.project / "missing");
    f.write("restored", "untracked obstruction");
    f.write("shape/untracked", "untracked descendant");
    fs::create_directories(f.project / "shape/empty/nested");
    f.write("outside.tmp", "unrelated untracked");
    f.write(".syrnckignore", "a\nshape\n*.tmp\n");
  }
};
void verify_target(Fixture &f, const Scenario &s) {
  require(f.store.refs().at("heads/main").id == s.target && !f.store.refs().contains("staging/main") &&
              text(read_bytes(f.project / "a")) == "target a" &&
              text(read_bytes(f.project / "missing")) == "target missing" &&
              text(read_bytes(f.project / "restored")) == "target restored" &&
              text(read_bytes(f.project / "shape")) == "target flat" && !fs::exists(f.project / "new") &&
              !fs::exists(f.project / "remove") &&
              text(read_bytes(f.project / "outside.tmp")) == "unrelated untracked",
          "hard reset lost target files, index reset or unrelated untracked work");
}
void workflow(const fs::path &path, std::optional<Key> key, unsigned physical) {
  Fixture f(path, key);
  const Scenario s(f);
  f.pack(physical);
  const auto before = f.physical();
  const auto plan = plan_native_hard_reset(f.store, path, "main", s.target, true);
  require(plan["ready"] == true && plan["workspace"]["namespace_transition"] == true &&
              plan["source_directories"] == Json::array({"shape", "shape/empty", "shape/empty/nested"}) &&
              plan["obstructing_untracked"] == Json::array({"restored", "shape/untracked"}) &&
              plan["working"].contains("a") && !plan["working"].contains("missing") &&
              !plan["working"].contains("outside.tmp") && f.physical() == before,
          "hard reset plan wrote data, followed ignores or omitted obstruction recovery");
  const auto id = plan["plan"].get<std::string>();
  require(plan_native_hard_reset(f.store, path, "main", s.target)["plan"] == id,
          "hard reset plan fence depends on audit mode");
  fails([&] { native_hard_reset(f.store, path, "main", s.target, s.target, id); });
  require(f.physical() == before, "wrong tip hard reset wrote objects");
  const auto timestamp = fs::last_write_time(path / "a");
  f.write("a", "Working a");
  fs::last_write_time(path / "a", timestamp);
  const auto changed = f.physical();
  fails([&] { native_hard_reset(f.store, path, "main", s.head, s.target, id); });
  require(f.physical() == changed, "stale content-fenced hard reset wrote objects");
  f.write("a", "working a");
  const auto applied = native_hard_reset(f.store, path, "main", s.head, s.target, id);
  require(applied["version"] == 5 && applied["mode"] == "hard" && applied["previous_index"] == s.index &&
              applied["original_head"] == s.head && applied["recovery_revision"] == applied["source"],
          "hard reset lost its versioned recovery/branch distinction");
  verify_target(f, s);
  f.store.gc(true);
  const auto recovery = applied["recovery_revision"].get<std::string>();
  f.store.export_revision(recovery, f.store.root() / "working");
  require(text(read_bytes(f.store.root() / "working/a")) == "working a" &&
              text(read_bytes(f.store.root() / "working/new")) == "working new" &&
              text(read_bytes(f.store.root() / "working/restored")) == "untracked obstruction" &&
              text(read_bytes(f.store.root() / "working/shape/untracked")) == "untracked descendant" &&
              !fs::exists(f.store.root() / "working/missing"),
          "hard reset GC lost pre-reset working bytes");
  f.store.export_revision(s.index, f.store.root() / "staged");
  require(text(read_bytes(f.store.root() / "staged/a")) == "staged a" &&
              text(read_bytes(f.store.root() / "staged/new")) == "staged new",
          "hard reset GC lost frozen index bytes");
  f.store.export_revision(s.head, f.store.root() / "head");
  require(text(read_bytes(f.store.root() / "head/a")) == "head a", "hard reset GC lost original history");
  require(f.store.fsck() > 0, "hard reset damaged Store");
  const auto stable = f.physical();
  const auto unchanged = plan_native_hard_reset(f.store, path, "main", s.target);
  require(unchanged["changed"] == false &&
              native_hard_reset(f.store, path, "main", s.target, s.target,
                                unchanged["plan"].get<std::string>())["state"] == "unchanged" &&
              f.physical() == stable,
          "clean same-head hard reset wrote data");
}
void recovery(const fs::path &path, std::string_view phase) {
  Fixture f(path);
  const Scenario s(f);
  const auto before = f.physical();
  const auto plan = plan_native_hard_reset(f.store, path, "main", s.target);
  bool paused = false;
  hook = [&](std::string_view event, const fs::path &) {
    if (!paused && event == phase) {
      paused = true;
      throw Error("controlled reset pause");
    }
  };
  fails([&] { native_hard_reset(f.store, path, "main", s.head, s.target, plan["plan"].get<std::string>()); });
  hook = {};
  require(paused && f.store.checkout_status()["pending"] == true, "hard reset never reached journal pause");
  if (phase == "checkout.committed") {
    fails([&] { f.store.recover_checkout(true); });
    f.store.recover_checkout();
    verify_target(f, s);
  } else {
    require(f.store.refs().at("heads/main").id == s.head && f.store.refs().at("staging/main").id == s.index,
            "interrupted reset published references early");
    f.store.recover_checkout(true);
    require(text(read_bytes(path / "a")) == "working a" && text(read_bytes(path / "new")) == "working new" &&
                text(read_bytes(path / "restored")) == "untracked obstruction" &&
                text(read_bytes(path / "shape/untracked")) == "untracked descendant" &&
                fs::is_directory(path / "shape/empty/nested") && !fs::exists(path / "missing") &&
                f.store.refs().at("staging/main").id == s.index,
            "hard reset abort did not restore dirty work, empty directories or the index");
  }
  require(f.store.checkout_status()["pending"] == false && f.store.fsck() > 0,
          "hard reset recovery left an active journal or damaged storage");
  (void)before;
}
void protected_obstruction(const fs::path &path) {
  Fixture f(path);
  const Scenario s(f);
  f.write("shape/.git/config", "protected nested Git data");
  const auto before = f.physical();
  const auto plan = plan_native_hard_reset(f.store, path, "main", s.target);
  require(plan["ready"] == false, "hard reset adopted protected storage");
  fails([&] { native_hard_reset(f.store, path, "main", s.head, s.target, plan["plan"].get<std::string>()); });
  require(f.physical() == before, "blocked hard reset wrote data");
}
void flat(const fs::path &path, std::optional<Key> key, unsigned layout) {
  Fixture f(path, key);
  const auto head = f.revision(Json{{"file", f.file("committed")}});
  f.install(head);
  f.write("file", "dirty");
  f.pack(layout);
  const auto plan = plan_native_hard_reset(f.store, path, "main", head, true);
  require(plan["ready"] == true && !plan["workspace"].value("namespace_transition", false),
          "ordinary hard reset unexpectedly requires namespace evacuation");
  const auto result = native_hard_reset(f.store, path, "main", head, head, plan["plan"].get<std::string>());
  require(result["version"] == 5 && result["namespace_transition"] == false &&
              text(read_bytes(path / "file")) == "committed",
          "ordinary hard reset lost its v5 profile");
  f.store.gc(true);
  f.store.export_revision(result["recovery_revision"].get<std::string>(), f.store.root() / "old");
  require(text(read_bytes(f.store.root() / "old/file")) == "dirty",
          "ordinary hard reset discarded dirty bytes");
  fs::remove(path / "file");
  const auto missing = plan_native_hard_reset(f.store, path, "main", head);
  native_hard_reset(f.store, path, "main", head, head, missing["plan"].get<std::string>());
  require(text(read_bytes(path / "file")) == "committed",
          "hard reset did not restore a missing tracked file");
}
void partial(const fs::path &base, std::optional<Key> key, unsigned physical,
             std::string_view pause = {}) {
  const auto path = base / "primary", edit = base / "edit";
  Fixture f(path, key);
  const auto target = f.revision(Json{{"Source/Code", f.file("target code")},
      {"Source/shape", f.file("target flat")}, {"Content/A/leaf", f.file("target leaf")},
      {"Content/B", f.file("target hidden")}});
  const auto head = f.revision(Json{{"Source/code", f.file("head code")},
      {"Source/shape/child", f.file("head child")}, {"Content/A/leaf", f.file("head leaf")},
      {"Content/B", f.file("head hidden")}}, Json::array({target}));
  f.install(head);
  f.store.create_native_branch("partial", head);
  f.store.register_partial_native_workspace(path, "edit", edit, {"Source", "Content/A/leaf"});
  f.store.use_native_workspace("edit");
  f.store.switch_directory(edit, "partial");
  const auto write = [&](std::string_view name, std::string_view value) {
    const auto file = edit / path_from_utf8(name);
    fs::create_directories(file.parent_path());
    atomic_write(file, bytes(value), true);
  };
  write("Source/code", "staged code");
  const auto index = f.store.stage_directory(edit, "partial", {"Source"});
  write("Source/code", "dirty code");
  write("Source/shape/local", "obstructing descendant");
  fs::create_directories(edit / "Source/shape/empty/deeper");
  fs::remove_all(edit / "Content");
  write("Content", "physical ancestor obstruction");
  write("outside/local", "unrelated excluded bytes");
  f.pack(physical);
  const auto before = f.physical();
  const auto plan = plan_native_hard_reset(f.store, edit, "partial", target, true);
  require(plan["version"] == 2 && plan["ready"] == true &&
              plan["materialization"] == Json::array({"Content/A/leaf", "Source"}) &&
              plan["working"].contains("Content") && !plan["working"].contains("Content/B") &&
              !plan["working"].contains("outside/local") && f.physical() == before,
          "partial reset mixed hidden logical children into physical ancestor recovery");
  require(plan_native_hard_reset(f.store, edit, "partial", target)["plan"] == plan["plan"],
          "partial reset audit changed the content fence");
  Json result;
  if (!pause.empty()) {
    bool paused = false;
    hook = [&](std::string_view event, const fs::path &) {
      if (!paused && event == pause) { paused = true; throw Error("partial reset pause"); }
    };
    fails([&] { native_hard_reset(f.store, edit, "partial", head, target, plan["plan"].get<std::string>()); });
    hook = {};
    require(paused && f.store.checkout_status()["pending"] == true,
            "partial reset did not reach the recovery checkpoint");
    if (pause != "checkout.committed") {
      f.store.recover_checkout(true);
      require(text(read_bytes(edit / "Content")) == "physical ancestor obstruction" &&
                  !fs::exists(edit / "Content/B") && text(read_bytes(edit / "Source/code")) == "dirty code" &&
                  text(read_bytes(edit / "Source/shape/local")) == "obstructing descendant" &&
                  fs::is_directory(edit / "Source/shape/empty/deeper") &&
                  f.store.refs().at("heads/partial").id == head &&
                  f.store.refs().at(native_staging_reference(f.store, "partial")).id == index &&
                  text(read_bytes(edit / "outside/local")) == "unrelated excluded bytes",
              "partial reset abort did not restore the physical obstruction and complete index");
      f.store.gc(true);
      f.store.fsck();
      return;
    }
    fails([&] { f.store.recover_checkout(true); });
    result = f.store.recover_checkout();
  } else {
    result = native_hard_reset(f.store, edit, "partial", head, target, plan["plan"].get<std::string>());
  }
  require(result["version"] == 9 && result["layout_version"] == 5 &&
              result["source_materialization"] == plan["materialization"] &&
              text(read_bytes(edit / "Content/A/leaf")) == "target leaf" && !fs::exists(edit / "Content/B") &&
              text(read_bytes(edit / "Source/Code")) == "target code" &&
              text(read_bytes(edit / "Source/shape")) == "target flat" &&
              text(read_bytes(edit / "outside/local")) == "unrelated excluded bytes" &&
              f.store.refs().at("heads/partial").id == target &&
              !f.store.refs().contains(native_staging_reference(f.store, "partial")),
          "partial hard reset did not atomically publish the projected target and full branch");
  f.store.gc(true);
  const auto recovery = result["source"].get<std::string>();
  const auto recovered = f.store.metadata(Kind::revision, recovery);
  require(recovered["parents"] == Json::array({head, index}) && recovered["files"].contains("Content") &&
              !recovered["files"].contains("Content/B"),
          "partial reset lost logical parents or corrupted the physical recovery namespace");
  f.store.export_revision(recovery, base / "working");
  f.store.export_revision(index, base / "index");
  f.store.export_revision(head, base / "head");
  require(text(read_bytes(base / "working/Content")) == "physical ancestor obstruction" &&
              text(read_bytes(base / "index/Content/B")) == "head hidden" &&
              text(read_bytes(base / "index/Source/code")) == "staged code" &&
              text(read_bytes(base / "head/Content/B")) == "head hidden",
          "partial hard reset lost recoverable physical, staged or hidden committed bytes after GC");
  create_backup(f.store, base / "backup", key);
  Store backup(base / "backup", key);
  verify_backup(backup);
  backup.export_revision(recovery, base / "backup-working");
  require(text(read_bytes(base / "backup-working/Content")) == "physical ancestor obstruction",
          "portable backup could not reconstruct partial hard reset recovery");
  f.store.fsck();
  f.store.detach_checkout(edit, head);
  write("Source/code", "detached dirty code");
  const auto detached_plan = plan_native_hard_reset(f.store, edit, detached_native_target, target, true);
  const auto detached_reset = native_hard_reset(f.store, edit, detached_native_target, head, target,
                                                detached_plan["plan"].get<std::string>());
  require(detached_reset["version"] == 9 && detached_reset["layout_version"] == 5 &&
              f.store.native_head()["branch"].is_null() && f.store.native_head()["revision"] == target &&
              f.store.refs().at("heads/partial").id == target &&
              text(read_bytes(edit / "Source/Code")) == "target code" &&
              text(read_bytes(edit / "outside/local")) == "unrelated excluded bytes" &&
              !fs::exists(edit / "Content/B"),
          "partial detached reset changed a named branch or materialized excluded files");
  f.store.gc(true);
  f.store.export_revision(detached_reset["source"].get<std::string>(), base / "detached-recovery");
  require(text(read_bytes(base / "detached-recovery/Source/code")) == "detached dirty code",
          "partial detached reset discarded local bytes");
}
void empty_partial(const fs::path &base) {
  Fixture f(base / "primary");
  const auto a = f.revision(Json{{"hidden", f.file("first hidden")}});
  const auto b = f.revision(Json{{"hidden", f.file("second hidden")}}, Json::array({a}));
  f.install(a);
  f.store.create_native_branch("empty", a);
  const auto edit = base / "empty";
  f.store.register_partial_native_workspace(f.project, "empty", edit, {});
  f.store.use_native_workspace("empty");
  f.store.switch_directory(edit, "empty");
  atomic_write(edit / "local", bytes("keep empty view data"), false);
  const auto plan = plan_native_hard_reset(f.store, edit, "empty", b);
  require(plan["version"] == 2 && plan["working"].empty() && plan["ready"] == true,
          "empty partial reset captured files outside its policy");
  const auto result = native_hard_reset(f.store, edit, "empty", a, b, plan["plan"].get<std::string>());
  require(result["version"] == 9 && result["namespace_transition"] == false &&
              !fs::exists(edit / "hidden") && text(read_bytes(edit / "local")) == "keep empty view data" &&
              f.store.refs().at("heads/empty").id == b,
          "empty partial hard reset materialized hidden files or modified unrelated data");
  f.store.gc(true);
  f.store.export_revision(a, base / "old");
  require(text(read_bytes(base / "old/hidden")) == "first hidden", "empty reset lost complete history");
}
} // namespace
namespace syrnck::testing {
void io_checkpoint(std::string_view phase, const fs::path &target) {
  if (hook)
    hook(phase, target);
}
} // namespace syrnck::testing
int main(int argc, char **argv) {
  Temporary temporary;
  try {
    fixture_test::TestCases cases(fixture_test::CaseSelection::parse(argc, argv));
    for (bool encrypted : {false, true})
      for (unsigned physical = 0; physical < 3; ++physical) {
        std::optional<Key> key;
        if (encrypted) {
          key.emplace();
          key->fill(74);
        }
        const auto suffix = std::to_string(encrypted) + '-' + std::to_string(physical);
        cases.add("workflow-" + suffix,
                  [key, physical](const fs::path &path) { workflow(path, key, physical); });
        cases.add("flat-" + suffix, [key, physical](const fs::path &path) { flat(path, key, physical); });
        cases.add("partial-" + suffix,
                  [key, physical](const fs::path &path) { partial(path, key, physical); });
      }
    for (const auto phase : {"checkout.evacuated", "checkout.applied", "checkout.committed"})
      cases.add("recovery-" + std::string(phase).substr(9),
                [phase](const fs::path &path) { recovery(path, phase); });
    for (const auto phase : {"checkout.evacuated", "checkout.applied", "checkout.committed"})
      cases.add("partial-recovery-" + std::string(phase).substr(9),
                [phase](const fs::path &path) { partial(path, {}, 0, phase); });
    cases.add("empty-partial", [](const fs::path &path) { empty_partial(path); });
    cases.add("protected-obstruction", [](const fs::path &path) { protected_obstruction(path); });
    return cases.run(temporary.path);
  } catch (const std::exception &error) {
    hook = {};
    std::cerr << error.what() << '\n';
    return 1;
  }
}
