#include "native_history_fixture.hpp"
#include <syrnck/stash.hpp>
#include <syrnck/workspace.hpp>
#include <syrnck/backup.hpp>
#include <syrnck/retention.hpp>
#include <iostream>
using namespace native_test;
namespace {
std::function<void(std::string_view, const fs::path &)> hook;
struct Scenario {
  std::string head, index;
  explicit Scenario(Fixture &f) {
    head = f.revision(Json{{"a", f.file("head a")},
                           {"missing", f.file("head missing")},
                           {"shape", f.file("head flat")},
                           {"keep", f.file("head keep")}});
    f.install(head);
    f.write("a", "staged a");
    f.write("added", "staged new");
    index = f.store.stage_directory(f.project, "main", {"a", "added"});
    f.write("a", "working a");
    f.write("added", "working new");
    fs::remove(f.project / "missing");
    fs::remove(f.project / "shape");
    f.write("shape/child", "working nested");
    f.write("outside", "unrelated work");
    f.write("ignored.tmp", "ignored work");
    f.write(".syrnckignore", "*.tmp\n");
  }
};
std::string save(Fixture &f, bool untracked = true) {
  const auto result = NativeStash(f.store).save(f.project, "main", "Saved work", {untracked});
  require(result["stash"].is_object(), "stash save unexpectedly found no work");
  return result["stash"]["id"].get<std::string>();
}
void apply(Fixture &f, const std::string &id, bool pop, NativeStashApplyOptions settings = {}) {
  NativeStash stash(f.store);
  const auto before = f.physical();
  const auto plan = stash.plan_apply(f.project, "main", id, settings, true);
  require(f.physical() == before && plan["ready"] == true, "stash apply plan wrote data or was blocked");
  require(stash.plan_apply(f.project, "main", id, settings)["plan"] == plan["plan"],
          "stash plan depends on audit mode");
  const auto result = stash.apply(f.project, "main", id, plan["plan"].get<std::string>(), pop, settings);
  require(result["version"] == 6 && result["pending"] == false, "stash apply did not finish v6 journal");
}
void workflow(const fs::path &path, std::optional<Key> key, unsigned layout) {
  Fixture f(path, key);
  const Scenario s(f);
  f.pack(layout);
  NativeStash stash(f.store);
  const auto id = save(f);
  require(text(read_bytes(path / "a")) == "head a" && text(read_bytes(path / "shape")) == "head flat" &&
              fs::exists(path / "missing") && !fs::exists(path / "added") && !fs::exists(path / "outside") &&
              !f.store.refs().contains("staging/main") && f.store.refs().at("heads/main").id == s.head &&
              text(read_bytes(path / "ignored.tmp")) == "ignored work",
          "stash save lost clean/index/ignored semantics");
  const auto shown = stash.show(id, true);
  require(shown["base"] == s.head && shown["index"] == s.index && stash.list()["total"] == 1,
          "stash lost base, original index or list record");
  NativeStashApplyOptions settings;
  settings.restore_index = true;
  apply(f, id, true, settings);
  const auto refs = f.store.refs();
  require(refs.at("heads/main").id == s.head && refs.contains("staging/main") &&
              text(read_bytes(path / "a")) == "working a" &&
              text(read_bytes(path / "added")) == "working new" &&
              text(read_bytes(path / "shape/child")) == "working nested" && !fs::exists(path / "missing") &&
              text(read_bytes(path / "outside")) == "unrelated work" && stash.list()["total"] == 0,
          "stash pop moved the head or lost working contents");
  const auto staged = f.store.metadata(Kind::revision, refs.at("staging/main").id);
  require(staged["files"]["a"]["manifest"] ==
              f.store.metadata(Kind::revision, s.index)["files"]["a"]["manifest"],
          "stash --index restored working contents into index");
  f.store.gc(true);
  require(stash.archived(id, true)["work"] == shown["work"], "stash pop lost retained work after GC");
  f.store.export_revision(s.index, f.store.root() / "original-index");
  require(text(read_bytes(f.store.root() / "original-index/a")) == "staged a", "stash GC lost index bytes");
  const auto backup_path = path.parent_path() / ("backup-" + utf8(path.filename()));
  const auto restored_path = path.parent_path() / ("restored-" + utf8(path.filename()));
  create_backup(f.store, backup_path, key);
  Store backup(backup_path, key);
  restore_backup(backup, restored_path, key);
  Store restored(restored_path, key);
  NativeStash restored_stash(restored);
  require(restored_stash.archived(id, true)["index"] == s.index, "backup lost archived stash/index");
  restored_stash.restore(id);
  require(restored_stash.show(id, true)["work"] == shown["work"],
          "restored backup lost active stash capability");
  const std::vector<std::string> retired{"archive/stash/" + shown["work"].get<std::string>()};
  const auto retirement = plan_archive_retirement(f.store, backup, retired);
  retire_archives(f.store, backup, retired, retirement["plan"].get<std::string>());
  f.store.gc(true);
  fails([&] { stash.archived(id, true); });
  for (const auto &[name, object] : f.store.refs())
    if (name.starts_with("checkouts/receipts/"))
      (void)f.store.checkout_status(name.substr(19));
}
void history_and_conflict(const fs::path &path) {
  Fixture f(path);
  const auto head = f.revision(Json{{"a", f.file("base")}, {"keep", f.file("base keep")}});
  f.install(head);
  f.write("a", "saved");
  const auto id = save(f, false);
  NativeStash stash(f.store);
  f.write("keep", "new keep");
  const auto newer = f.store.commit_directory(path, "main", "advance");
  apply(f, id, false);
  require(f.store.refs().at("heads/main").id == newer && text(read_bytes(path / "a")) == "saved" &&
              text(read_bytes(path / "keep")) == "new keep",
          "stash lost changes from newer head");
  const auto reset = plan_native_hard_reset(f.store, path, "main", newer);
  native_hard_reset(f.store, path, "main", newer, newer, reset["plan"].get<std::string>());
  f.write("a", "conflicting");
  const auto conflict = f.store.commit_directory(path, "main", "conflicting head");
  const auto before = f.physical();
  const auto plan = stash.plan_apply(path, "main", id, {}, true);
  require(plan["ready"] == false && plan["working"]["conflicts"].size() == 1 && f.physical() == before,
          "stash silently resolved conflicting edits");
  fails([&] { stash.apply(path, "main", id, plan["plan"].get<std::string>()); });
  require(f.physical() == before, "blocked stash wrote objects");
  NativeStashApplyOptions options;
  options.resolutions = NativeResolutionSet::parse(plan["working"]["resolution_context"]);
  options.resolutions->files["a"] = {NativeResolutionChoice::incoming};
  apply(f, id, true, options);
  require(text(read_bytes(path / "a")) == "saved" && f.store.refs().at("heads/main").id == conflict,
          "stash explicit resolution moved branch or lost choice");
}
void stale_index_and_text(const fs::path &path) {
  Fixture f(path);
  const auto original = f.revision(Json{{"a", f.file("base a\n")}, {"keep", f.file("base keep\n")}});
  f.install(original);
  f.write("a", "staged a\n");
  const auto index = f.store.stage_directory(path, "main", {"a"});
  const auto advanced = f.revision(Json{{"a", f.file("base a\n")}, {"keep", f.file("advanced keep\n")}},
                                   Json::array({original}));
  f.store.set_ref("heads/main", {Kind::revision, advanced});
  f.write("keep", "advanced keep\n");
  f.write("a", "working a\n");
  const auto id = save(f, false);
  NativeStash stash(f.store);
  NativeStashApplyOptions settings;
  settings.restore_index = true;
  const auto before = f.physical();
  const auto initial = stash.plan_apply(path, "main", id, settings);
  require(initial["ready"] == true,
          "stale stash index was compared to stash base instead of original index base");
  const auto staged = initial["target_index"].get<std::string>();
  apply(f, id, false, settings);
  const auto metadata = f.store.metadata(Kind::revision, staged);
  require(metadata["files"]["keep"] == f.store.metadata(Kind::revision, advanced)["files"]["keep"] &&
              metadata["files"]["a"] == f.store.metadata(Kind::revision, index)["files"]["a"],
          "stale stash index reverted changes already in current head");
  const auto reset = plan_native_hard_reset(f.store, path, "main", advanced);
  native_hard_reset(f.store, path, "main", advanced, advanced, reset["plan"].get<std::string>());
  f.write("a", "base a\n");
  f.write("text", "one\ntwo\nthree\n");
  f.store.commit_directory(path, "main", "text base");
  f.write("text", "ONE\ntwo\nthree\n");
  const auto text_id = save(f, false);
  f.write("text", "one\ntwo\nTHREE\n");
  const auto head = f.store.commit_directory(path, "main", "other line");
  const auto conflict = stash.plan_apply(path, "main", text_id);
  require(conflict["ready"] == false, "stash silently merged text without request");
  NativeStashApplyOptions text_settings;
  text_settings.text_merge.emplace();
  const auto clean = f.physical();
  const auto merged = stash.plan_apply(path, "main", text_id, text_settings, true);
  require(merged["ready"] == true && f.physical() == clean, "stash text plan wrote candidate objects");
  apply(f, text_id, true, text_settings);
  require(text(read_bytes(path / "text")) == "ONE\ntwo\nTHREE\n" &&
              f.store.refs().at("heads/main").id == head,
          "stash text merge lost independent line edits or moved head");
  (void)before;
}
void scanner_bounds(const fs::path &normal) {
#ifdef _WIN32
  const fs::path path(std::wstring(L"\\\\?\\") + fs::absolute(normal).native());
#else
  const auto &path = normal;
#endif
  {
    Fixture f(path);
    const auto head = f.revision(Json{{"a", f.file("base")}});
    f.install(head);
    f.write("a", "work");
    auto nested = path;
    for (unsigned depth = 0; depth < 129; ++depth) {
      nested /= "d";
      fs::create_directory(nested);
    }
    f.write(nested.lexically_relative(path) / "file", "deep");
    const auto before = f.physical();
    bool rejected = false;
    try {
      NativeStash(f.store).save(path, "main", "deep", {true});
    } catch (const Error &error) {
      rejected = std::string_view(error.what()) == "native workspace directory nesting exceeds 128 levels";
#ifdef _WIN32
    } catch (const fs::filesystem_error &error) {
      // The Windows host can impose its legacy path limit before 128 levels.
      // Verify fail-closed/no writes there; the depth assertion runs on hosts
      // where the filesystem permits this fixture's full canonical path.
      rejected = error.code() == std::errc::no_such_file_or_directory ||
                 error.code() == std::errc::filename_too_long;
      require(rejected, "unexpected Windows deep-path failure");
      std::cout << "Windows legacy path limit reached before scanner depth limit\n";
#endif
    }
    require(rejected && f.physical() == before,
            "stash scanner exceeded depth budget or wrote before rejecting");
  }
  fs::remove_all(path);
}
void default_untracked_and_registry(const fs::path &path) {
  Fixture f(path);
  const auto head = f.revision(Json{{"a", f.file("base")}});
  f.install(head);
  NativeStash stash(f.store);
  const auto before = f.physical();
  require(stash.save(path, "main", "clean")["stash"].is_null() && f.physical() == before,
          "clean stash wrote metadata");
  f.write("outside", "untracked");
  f.write("a", "one");
  const auto one = save(f, false);
  require(text(read_bytes(path / "outside")) == "untracked", "default stash removed untracked work");
  f.write("a", "two");
  const auto two = save(f, false);
  const auto first = stash.list({}, 1);
  require(first["stashes"][0]["id"] == two && first["has_more"] == true &&
              stash.list(first["next_before"].get<std::uint64_t>(), 1)["stashes"][0]["id"] == one,
          "stash pagination order or cursor changed");
  fails([&] { stash.drop(one, std::string(64, 'a')); });
  stash.drop(one, stash.show(one)["record"].get<std::string>());
  f.store.gc(true);
  require(stash.archived(one, true)["id"] == one, "stash drop lost retained work after GC");
  stash.restore(one);
  require(stash.list()["stashes"][1]["id"] == one, "stash restore changed generation order");
  fails([&] { stash.restore(one); });
  fails([&] { stash.list({}, 0); });
  const auto clock = f.store.refs().at("stash/clock");
  for (const auto &bad : {std::string("{\"version\":1.0,\"type\":\"native-stash-clock\",\"value\":2}"),
                          std::string("{\"version\":1,\"type\":\"native-stash-clock\",\"value\":0}")}) {
    f.store.set_ref("stash/clock", f.store.put(Kind::chunk, bytes(bad)).first);
    const auto invalid = f.physical();
    fails([&] { stash.list(); });
    require(f.physical() == invalid, "invalid stash clock validation wrote data");
  }
  f.store.set_ref("stash/clock", clock);
  const auto stable = f.physical();
  fails([&] { stash.save(path, "main", std::string(1024 * 1024 + 1, 'm')); });
  require(f.physical() == stable, "oversized stash message wrote objects");
  f.write("a", "dirty");
  const auto blocked = stash.plan_apply(path, "main", one);
  require(blocked["ready"] == false, "stash overwrote dirty workspace");
}
void recovery(const fs::path &path, std::string_view phase, bool abort) {
  Fixture f(path);
  const Scenario s(f);
  const auto id = save(f);
  NativeStash stash(f.store);
  NativeStashApplyOptions options;
  options.restore_index = true;
  const auto plan = stash.plan_apply(path, "main", id, options);
  bool paused = false;
  hook = [&](std::string_view event, const fs::path &) {
    if (!paused && event == phase) {
      paused = true;
      throw Error("controlled stash pause");
    }
  };
  fails([&] { stash.apply(path, "main", id, plan["plan"].get<std::string>(), true, options); });
  hook = {};
  require(paused && f.store.checkout_status()["pending"] == true, "stash journal pause was not reached");
  if (phase == "checkout.committed") {
    fails([&] { f.store.recover_checkout(true); });
    f.store.recover_checkout();
  } else
    f.store.recover_checkout(abort);
  require(f.store.refs().at("heads/main").id == s.head, "stash recovery moved branch");
  if (abort) {
    require(text(read_bytes(path / "a")) == "head a" && stash.list()["total"] == 1 &&
                !f.store.refs().contains("staging/main"),
            "stash abort lost clean work or removed stash early");
  } else {
    require(text(read_bytes(path / "a")) == "working a" && stash.list()["total"] == 0 &&
                f.store.refs().contains("staging/main"),
            "stash resume split working/index/pop publication");
  }
  f.store.gc(true);
  if (!abort)
    require(stash.archived(id, true)["base"] == s.head, "stash recovery lost archive");
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
      for (unsigned layout = 0; layout < 3; ++layout) {
        std::optional<Key> key;
        if (encrypted) {
          key.emplace();
          key->fill(76);
        }
        workflow(temporary.path / (std::to_string(encrypted) + '-' + std::to_string(layout)), key, layout);
      }
    stale_index_and_text(temporary.path / "stale-index-text");
    scanner_bounds(temporary.path / "scanner");
    history_and_conflict(temporary.path / "conflicts");
    default_untracked_and_registry(temporary.path / "registry");
    for (const auto phase :
         {"checkout.prepared", "checkout.evacuated", "checkout.applied", "checkout.committed"}) {
      recovery(temporary.path / (std::string(phase) + "-resume"), phase, false);
      if (phase != std::string_view("checkout.committed"))
        recovery(temporary.path / (std::string(phase) + "-abort"), phase, true);
    }
    std::cout << "Native stash tests passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
