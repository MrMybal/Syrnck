#include "native_history_fixture.hpp"
#include <syrnck/workspace.hpp>
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
} // namespace
namespace syrnck::testing {
void io_checkpoint(std::string_view phase, const fs::path &target) {
  if (hook)
    hook(phase, target);
}
} // namespace syrnck::testing
int main() {
  Temporary temporary;
  try {
    for (bool encrypted : {false, true})
      for (unsigned physical = 0; physical < 3; ++physical) {
        std::optional<Key> key;
        if (encrypted) {
          key.emplace();
          key->fill(74);
        }
        workflow(temporary.path / (std::to_string(encrypted) + '-' + std::to_string(physical)), key,
                 physical);
        flat(temporary.path / ("flat-" + std::to_string(encrypted) + '-' + std::to_string(physical)), key,
             physical);
      }
    for (const auto phase : {"checkout.evacuated", "checkout.applied", "checkout.committed"})
      recovery(temporary.path / phase, phase);
    protected_obstruction(temporary.path / "protected");
    std::cout << "Hard reset captures dirty/staged/obstructing bytes and restores namespace on abort\n";
  } catch (const std::exception &error) {
    hook = {};
    std::cerr << error.what() << '\n';
    return 1;
  }
}
