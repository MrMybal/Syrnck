#include "native_history_fixture.hpp"
#include <syrnck/stash.hpp>
#include <syrnck/workspace.hpp>
#include <syrnck/backup.hpp>
#include <iostream>
using namespace native_test;
namespace {
void scenario(const fs::path &path, std::optional<Key> key, unsigned layout) {
  Fixture f(path, key);
  const auto a = f.revision(Json{{"code", f.file("first\ncommon\n")}});
  const auto b = f.revision(Json{{"code", f.file("main\ncommon\n")}}, Json::array({a}));
  f.install(b);
  f.pack(layout);
  const auto untouched = f.physical();
  const auto plan = f.store.plan_detached_checkout(path, a, true);
  require(plan["ready"] == true && plan["source"] == b && plan["target"] == a &&
              plan["operation"] == "switch-detached" && plan["destination_branch"].is_null(),
          "detached checkout plan differs");
  require(f.physical() == untouched, "detached preflight wrote stored state");
  const auto result = f.store.detach_checkout(path, a);
  const auto head = f.store.native_head();
  const auto target = f.store.native_target();
  require(head["branch"].is_null() && head["mode"] == "detached" && head["revision"] == a &&
              head["explicit"] == true && target == ":HEAD" &&
              result["version"] == 7 && result["state"] == "committed",
          "detached selection is not independent of named branches");
  require(text(read_bytes(path / "code")) == "first\ncommon\n" &&
              f.store.refs().at("heads/main").id == b &&
              f.store.refs().at("workspace/detached").id == a,
          "detached checkout moved a named branch or restored wrong bytes");
  require(f.store.resolve_native_revision("HEAD") == a && f.store.resolve_native_revision(target) == a,
          "detached HEAD revision resolution differs");
  const auto branches = f.store.native_branches();
  for (const auto &row : branches["branches"])
    require(row["current"] == false, "detached HEAD selected a named branch");
  f.write("code", "detached\ncommon\n");
  const auto c = f.store.commit_directory(path, target, "detached child");
  require(f.store.native_head()["revision"] == c && f.store.metadata(Kind::revision, c)["parents"] == Json::array({a}) &&
              f.store.refs().at("heads/main").id == b, "detached commit moved main or lost its parent");
  f.write("code", "staged\ncommon\n");
  const auto index = f.store.stage_directory(path, target, {"code"});
  require(f.store.refs().contains("workspace/staging") && !f.store.refs().contains("staging/:HEAD"),
          "detached index is not a private retained root");
  f.write("code", "later\ncommon\n");
  const auto combined = f.store.workspace_status(path, target);
  require(combined["staged"]["snapshot"] == index && combined["staged"]["clean"] == false &&
              combined["unstaged"]["clean"] == false, "detached status lost independent staging");
  const auto d = f.store.commit_staged(target, "detached staged child");
  require(!f.store.refs().contains("workspace/staging") && f.store.native_head()["revision"] == d &&
              f.store.metadata(Kind::revision, d)["parents"] == Json::array({c}),
          "detached staged commit did not consume its private index");
  NativeStash stash(f.store);
  const auto saved = stash.save(path, target, "detached work");
  const auto id = saved["stash"]["id"].get<std::string>();
  require(text(read_bytes(path / "code")) == "staged\ncommon\n" && stash.show(id)["branch"] == target,
          "detached stash did not clean tracked working changes");
  const auto apply = stash.plan_apply(path, target, id);
  require(apply["ready"] == true, "detached stash replay was blocked");
  stash.apply(path, target, id, apply["plan"].get<std::string>(), true);
  require(text(read_bytes(path / "code")) == "later\ncommon\n" && f.store.native_head()["revision"] == d,
          "detached stash pop moved HEAD or lost working bytes");
  const auto reset = plan_native_hard_reset(f.store, path, target, a);
  native_hard_reset(f.store, path, target, d, a, reset["plan"].get<std::string>());
  require(f.store.native_head()["revision"] == a && text(read_bytes(path / "code")) == "first\ncommon\n",
          "detached hard reset did not use the private revision root");
  f.store.create_native_tag("detached", c);
  f.store.create_native_branch("rescue", d);
  require(f.store.native_tag("detached")["tag"]["revision"] == c,
          "detached commits cannot be retained by tags");
  f.store.switch_directory(path, "main");
  require(f.store.native_head()["branch"] == "main" && f.store.native_head()["revision"] == b &&
              text(read_bytes(path / "code")) == "main\ncommon\n", "reattaching did not restore main");
  f.store.detach_checkout(path, c);
  require(f.store.refs().contains("archive/" + a) && f.store.native_head()["revision"] == c,
          "replacing an inactive detached line did not retain its old tip");
  require(f.store.native_archive("archive/" + a, true)["archive"]["kind"] == "detached",
          "replaced detached history is missing from the native archive inventory");
  f.store.gc(true);
  f.store.export_revision(d, path / "retained-staged");
  require(text(read_bytes(path / "retained-staged/code")) == "staged\ncommon\n",
          "detached staging history was lost after GC");
  const auto receipts = f.store.checkout_status(result["id"].get<std::string>());
  require(receipts["version"] == 7 && receipts["state"] == "committed",
          "old detached receipts cannot be inspected after later selection changes/GC");
  fails([&] { f.store.create_native_branch(target, c); });
  // Backup drops the machine-specific HEAD record, retaining private roots.
  const auto backup_path = path.parent_path() / ("backup-" + utf8(path.filename()));
  create_backup(f.store, backup_path, key);
  Store backup(backup_path, key);
  require(!backup.refs().contains("workspace/HEAD") && backup.refs().at("workspace/detached").id == c,
          "portable backup discarded detached data or kept a machine binding");
  backup.export_revision(c, backup_path / "saved");
  require(text(read_bytes(backup_path / "saved/code")) == "detached\ncommon\n",
          "detached backup cannot reconstruct its retained revision");
}
void failures(const fs::path &path) {
  Fixture f(path);
  const auto a = f.revision(Json{{"code", f.file("first")}});
  const auto b = f.revision(Json{{"code", f.file("second")}}, Json::array({a}));
  f.install(b);
  f.write("code", "dirty");
  const auto before = f.physical();
  require(f.store.plan_detached_checkout(path, a)["ready"] == false, "dirty detached preflight was ready");
  fails([&] { f.store.detach_checkout(path, a); });
  require(f.physical() == before && text(read_bytes(path / "code")) == "dirty",
          "blocked detached checkout modified data");
  f.write("code", "second");
  f.store.detach_checkout(path, a);
  const auto selection = f.store.refs().at("workspace/HEAD");
  auto bad = Json{{"version", 1}, {"type", "native-head"}, {"directory", utf8(fs::canonical(path))},
                  {"branch", ":HEAD"}};
  f.store.set_ref("workspace/HEAD", f.store.put(Kind::chunk, bytes(bad.dump())).first);
  fails([&] { f.store.native_head(); });
  bad["version"] = 2;
  bad["branch"] = "main";
  f.store.set_ref("workspace/HEAD", f.store.put(Kind::chunk, bytes(bad.dump())).first);
  fails([&] { f.store.native_head(); });
  f.store.set_ref("workspace/HEAD", selection);
  f.store.remove_ref("workspace/detached");
  fails([&] { f.store.native_head(); });
  fails([&] { f.store.native_target(); });
}
void interrupted(const fs::path &path, bool encrypted, unsigned action, bool abort) {
  std::optional<Key> key;
  if (encrypted) { key.emplace(); key->fill(33); }
  std::string base, main, index, receipt;
  {
    Fixture f(path, key);
    base = f.revision(Json{{"asset", f.file("base")}, {"shape", f.file("flat")}});
    main = f.revision(Json{{"asset", f.file("main")}, {"shape/child", f.file("nested")}}, Json::array({base}));
    f.install(main);
    f.store.detach_checkout(path, base);
    const auto target = f.store.native_target();
    f.write("asset", "prepared");
    index = f.store.stage_directory(path, target, {"asset"});
    f.write("asset", "working");
    fs::remove(path / "shape");
    f.write("shape/child", "working child");
    NativeStash stash(f.store);
    std::string stash_id;
    if (action == 2)
      stash_id = stash.save(path, target, "before interrupted apply", {true})["stash"]["id"].get<std::string>();
    std::stop_source stop;
    const OperationOptions options{stop.get_token(), [&](const ProgressEvent &event) {
      if (event.phase == ProgressPhase::publishing && event.path == "checkout") stop.request_stop();
    }};
    bool cancelled = false;
    try {
      if (action == 0) {
        const auto plan = plan_native_hard_reset(f.store, path, target, main);
        native_hard_reset(f.store, path, target, base, main, plan["plan"].get<std::string>(), options);
      } else if (action == 1)
        stash.save(path, target, "interrupted save", {true}, options);
      else {
        NativeStashApplyOptions settings; settings.restore_index = true;
        const auto plan = stash.plan_apply(path, target, stash_id, settings);
        stash.apply(path, target, stash_id, plan["plan"].get<std::string>(), true, settings, options);
      }
    } catch (const Cancelled &) { cancelled = true; }
    const auto pending = f.store.checkout_status();
    require(cancelled && pending["pending"] == true && pending["version"] == 7 &&
                pending["layout_version"] == (action == 2 ? 6 : 5),
            "detached reset/stash did not retain its pending versioned record");
    receipt = pending["id"].get<std::string>();
  }
  // A fresh handle parses the new layouts and recovers using retained HEAD v2.
  Store store(path / ".syrnck", key);
  store.recover_checkout(abort);
  const bool dirty = action == 2 ? !abort : abort;
  const auto expected_asset = dirty ? "working" : action == 0 && !abort ? "main" : "base";
  require(text(read_bytes(path / "asset")) == expected_asset &&
              fs::is_directory(path / "shape") == (dirty || (action == 0 && !abort)),
          "detached reset/stash recovery lost working bytes or namespace");
  if (dirty)
    require(text(read_bytes(path / "shape/child")) == "working child",
            "detached recovery lost dirty namespace bytes");
  const auto refs = store.refs();
  require(refs.at("heads/main").id == main &&
              store.native_head()["revision"] == (action == 0 && !abort ? main : base) &&
              store.native_head()["branch"].is_null() && refs.contains("workspace/staging") == dirty,
          "detached reset/stash recovery split HEAD, branch or index");
  if (dirty)
    require(store.metadata(Kind::revision, refs.at("workspace/staging").id)["files"] ==
                store.metadata(Kind::revision, index)["files"], "detached recovery changed prepared bytes");
  store.gc(true);
  require(store.checkout_status()["pending"] == false, "detached reset/stash recovery left an active journal");
  if (!abort)
    require(store.checkout_status(receipt)["state"] == "committed",
            "completed detached reset/stash receipt became unreadable after GC");
  store.fsck();
}
} // namespace
int main() {
  Temporary temporary;
  try {
    for (bool encrypted : {false, true})
      for (unsigned layout = 0; layout < 3; ++layout) {
        std::optional<Key> key;
        if (encrypted) { key.emplace(); key->fill(32); }
        scenario(temporary.path / (std::to_string(encrypted) + '-' + std::to_string(layout)), key, layout);
      }
    failures(temporary.path / "failures");
    for (bool encrypted : {false, true})
      for (unsigned action = 0; action < 3; ++action)
        for (bool abort : {false, true})
          interrupted(temporary.path / ("interrupted-" + std::to_string(encrypted) + '-' +
              std::to_string(action) + '-' + std::to_string(abort)), encrypted, action, abort);
    std::cout << "Detached HEAD, private index, stash/reset, reattachment, retained GC and backups\n";
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
