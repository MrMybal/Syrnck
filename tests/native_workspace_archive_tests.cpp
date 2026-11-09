#include "native_history_fixture.hpp"
#include "test_cases.hpp"
#include "native_target.hpp"
#include "native_workspace_archive.hpp"
#include <syrnck/workspace.hpp>
#include <syrnck/backup.hpp>
#include <syrnck/retention.hpp>
#include <iostream>
using namespace native_test;
namespace {
std::map<std::string, std::string> working(const fs::path &directory) {
  std::map<std::string, std::string> result;
  for (const auto &entry : fs::recursive_directory_iterator(directory))
    if (entry.is_regular_file()) result.emplace(utf8(entry.path().lexically_relative(directory)), text(read_bytes(entry.path())));
  return result;
}
void scenario(const fs::path &base, std::optional<Key> key, unsigned physical, bool partial, bool detached) {
  const auto primary = base / "primary", edit = base / "edit";
  Fixture f(primary, key);
  const auto a = f.revision(Json{{".syrnckignore", f.file("*.tmp\n")}, {"Source/code", f.file("base code")},
      {"Source/shape", f.file("flat")}, {"Content/A/leaf", f.file("base leaf")}, {"Content/B", f.file("base hidden")}});
  auto next = f.store.metadata(Kind::revision, a)["files"];
  next["Content/B"] = f.file("head hidden");
  const auto head = f.revision(next, Json::array({a}));
  f.install(a);
  f.store.create_native_branch("feature", head);
  if (partial) f.store.register_partial_native_workspace(primary, "edit", edit, {"Source", "Content/A/leaf"});
  else f.store.register_native_workspace(primary, "edit", edit);
  f.store.use_native_workspace("edit");
  if (detached) f.store.detach_checkout(edit, head);
  else f.store.switch_directory(edit, "feature");
  const auto target = std::string(f.store.native_target());
  const auto write = [&](std::string_view name, std::string_view value) {
    const auto path = edit / path_from_utf8(name);
    fs::create_directories(path.parent_path()); atomic_write(path, bytes(value), true);
  };
  write("Source/code", "staged code");
  write("Source/added", "staged addition");
  const auto index = f.store.stage_directory(edit, target, {"Source"});
  write("Source/code", "dirty code");
  write("Source/added", "dirty addition");
  fs::remove(edit / "Source/shape");
  write("Source/shape/child", "dirty nested");
  write("Source/untracked", "selected untracked");
  write("outside", "outside work");
  write("ignored.tmp", "ignored work");
  fs::remove_all(edit / "Content");
  write("Content", "physical ancestor");
  const auto retained = f.revision(Json{{"only", f.file("inactive retained bytes")}});
  f.store.set_ref("workspaces/edit/retained/extra", {Kind::revision, retained});
  f.pack(physical);
  const auto before = working(edit), storage = f.physical();
  const auto plan = plan_native_workspace_unregistration(f.store, true, true);
  require(plan["ready"] == true && working(edit) == before && f.physical() == storage,
      "workspace archive plan wrote data or omitted physical ancestor recovery");
  const auto result = unregister_native_workspace(f.store, plan["plan"].get<std::string>(), true);
  const auto id = result["archive"].get<std::string>();
  require(result["state"] == "unregistered" && f.store.native_workspace_name().empty() &&
              working(edit) == before, "workspace unregistration changed files or left its context selected");
  const auto refs = f.store.refs();
  for (const auto &[name, object] : refs) { (void)object; require(!name.starts_with("workspaces/edit/"), "workspace root escaped archival"); }
  require(refs.at("heads/main").id == a && refs.at("heads/feature").id == head,
      "workspace unregistration changed shared branches");
  fails([&] { f.store.use_native_workspace("edit"); });
  auto shown = native_workspace_archive(f.store, id, true);
  require(shown["revision"] == head && shown["index"] == index &&
              native_workspace_archives(f.store)["archives"][0]["id"] == id,
      "workspace archive lost head, independent index or inventory");
  f.store.gc(true);
  for (const auto &[revision, folder] : std::vector<std::pair<std::string, std::string>>{
      {head, "head"}, {index, "index"}, {shown["work"].get<std::string>(), "work"}, {retained, "retained"}})
    f.store.export_revision(revision, base / folder);
  require(text(read_bytes(base / "head/Content/B")) == "head hidden" &&
              text(read_bytes(base / "index/Source/code")) == "staged code" &&
              text(read_bytes(base / "work/Content")) == "physical ancestor" &&
              text(read_bytes(base / "retained/only")) == "inactive retained bytes" &&
              !fs::exists(base / "work/ignored.tmp") && fs::exists(base / "work/outside") == !partial,
      "archive GC lost complete history, indexes, physical ancestors or inactive roots");
  const auto destination = base / "restored";
  NativeWorkspaceRestoreOptions settings;
  settings.detached = true;
  const auto restore = plan_native_workspace_archive_restore(f.store, primary, id, "restored", destination, settings, true);
  const auto applied = restore_native_workspace_archive(f.store, primary, id, "restored", destination,
      restore["plan"].get<std::string>(), settings);
  require(applied["state"] == "restored" && text(read_bytes(destination / "Content")) == "physical ancestor" &&
              text(read_bytes(destination / "Source/code")) == "dirty code" &&
              text(read_bytes(destination / "Source/added")) == "dirty addition" &&
              text(read_bytes(destination / "Source/untracked")) == "selected untracked",
      "workspace archive restore lost dirty work or physical namespace");
  const auto stable = f.physical();
  restore_native_workspace_archive(f.store, primary, id, "restored", destination, restore["plan"].get<std::string>(), settings);
  require(f.physical() == stable, "idempotent workspace restoration wrote data");
  f.store.use_native_workspace("restored");
  require(f.store.native_head()["revision"] == head && f.store.native_head()["branch"].is_null(),
      "workspace archive restored working tree as logical HEAD");
  const auto restored_index = f.store.refs().at(native_staging_reference(f.store, detached_native_target));
  require(f.store.metadata(Kind::revision, restored_index.id)["files"] == f.store.metadata(Kind::revision, index)["files"],
      "workspace archive restored dirty contents into the frozen index");
  f.store.use_native_workspace("");
  create_backup(f.store, base / "backup", key);
  Store backup(base / "backup", key);
  verify_backup(backup);
  require(native_workspace_archive(backup, id, true)["work"] == shown["work"], "backup lost archived workspace state");
  const auto group = "archive/workspaces/" + id;
  const auto retirement = plan_archive_retirement(f.store, backup, {group});
  retire_archives(f.store, backup, {group}, retirement["plan"].get<std::string>());
  fails([&] { native_workspace_archive(f.store, id); });
  f.store.gc(true); f.store.fsck();
  f.store.export_revision(shown["work"].get<std::string>(), base / "restored-recovery");
  require(text(read_bytes(base / "restored-recovery/Content")) == "physical ancestor",
      "restored context lost its working recovery snapshot after source archive retirement");
  require(native_workspace_archive(backup, id, true)["index"] == index, "archive retirement invalidated its verified backup");
  const auto recovered = base / "recovered";
  fs::create_directory(recovered);
  restore_backup(backup, recovered / ".syrnck", key);
  Store recovered_store(recovered / ".syrnck", key);
  const auto copied_view = base / "from-backup";
  const auto copied_plan = plan_native_workspace_archive_restore(recovered_store, recovered, id, "from-archive", copied_view, settings, true);
  restore_native_workspace_archive(recovered_store, recovered, id, "from-archive", copied_view,
      copied_plan["plan"].get<std::string>(), settings);
  recovered_store.use_native_workspace("from-archive");
  require(recovered_store.native_head()["revision"] == head &&
              text(read_bytes(copied_view / "Source/code")) == "dirty code" &&
              text(read_bytes(copied_view / "Content")) == "physical ancestor",
      "portable backup restore lost archived dirty work or logical HEAD");
  recovered_store.gc(true); recovered_store.fsck();
}
void refusals(const fs::path &base) {
  const auto primary = base / "primary", edit = base / "edit";
  Fixture f(primary);
  const auto head = f.revision(Json{{"Source/code", f.file("base")}});
  const auto other = f.revision(Json{{"Source/code", f.file("next")}}, Json::array({head}));
  f.install(head); f.store.create_native_branch("feature", head);
  fails([&] { plan_native_workspace_unregistration(f.store); });
  f.store.register_native_workspace(primary, "edit", edit);
  f.store.use_native_workspace("edit"); f.store.switch_directory(edit, "feature");
  std::stop_source stop;
  fails([&] { f.store.checkout_directory(edit, "feature", other, {stop.get_token(), [&](const ProgressEvent &event) {
    if (event.phase == ProgressPhase::publishing && event.path == "checkout") stop.request_stop();
  }}); });
  require(f.store.checkout_status()["pending"] == true, "workspace archive pending-checkout fixture did not stop");
  auto refs = f.store.refs();
  fails([&] { plan_native_workspace_unregistration(f.store); });
  require(f.store.refs() == refs, "blocked unregistration changed pending checkout roots");
  f.store.recover_checkout(true);
  const auto original = plan_native_workspace_unregistration(f.store);
  atomic_write(edit / "Source/code", bytes("BASE"), true);
  refs = f.store.refs();
  fails([&] { unregister_native_workspace(f.store, original["plan"].get<std::string>()); });
  require(f.store.refs() == refs && text(read_bytes(edit / "Source/code")) == "BASE", "stale unregistration changed work or references");
  const auto plan = plan_native_workspace_unregistration(f.store);
  bool changed = false;
  fails([&] { unregister_native_workspace(f.store, plan["plan"].get<std::string>(), false,
      {{}, [&](const ProgressEvent &event) {
        if (!changed && event.phase == ProgressPhase::publishing && event.path == "workspace-unregister") {
          changed = true; atomic_write(edit / "Source/code", bytes("last observer"), true);
        }
      }}); });
  require(changed && f.store.refs() == refs && native_workspace_archives(f.store)["archives"].empty() &&
              text(read_bytes(edit / "Source/code")) == "last observer", "unregistration ignored its final content fence");
  const auto ready = plan_native_workspace_unregistration(f.store);
  const auto result = unregister_native_workspace(f.store, ready["plan"].get<std::string>());
  const auto id = result["archive"].get<std::string>();
  f.store.set_ref("heads/feature", {Kind::revision, other});
  fails([&] { plan_native_workspace_archive_restore(f.store, primary, id, "restored", base / "restored"); });
  const auto occupied = base / "occupied";
  fs::create_directory(occupied); atomic_write(occupied / "unrelated", bytes("keep"), false);
  NativeWorkspaceRestoreOptions settings; settings.detached = true;
  refs = f.store.refs();
  fails([&] { plan_native_workspace_archive_restore(f.store, primary, id, "occupied", occupied, settings); });
  require(f.store.refs() == refs && text(read_bytes(occupied / "unrelated")) == "keep", "restore adopted an unrelated directory");
  const auto destination = base / "callback";
  const auto restore = plan_native_workspace_archive_restore(f.store, primary, id, "callback", destination, settings);
  changed = false;
  fails([&] { restore_native_workspace_archive(f.store, primary, id, "callback", destination,
      restore["plan"].get<std::string>(), settings, {{}, [&](const ProgressEvent &event) {
        if (!changed && event.phase == ProgressPhase::publishing && event.path == "workspace-archive-restore") {
          changed = true; atomic_write(destination / "Source/code", bytes("preserve external edit"), true);
        }
      }}); });
  require(changed && f.store.refs() == refs && text(read_bytes(destination / "Source/code")) == "preserve external edit",
      "restore registered a directory changed by its final observer");
  NativeWorkspaceRestoreOptions named; named.branch = "recovered-branch";
  const auto named_directory = base / "named";
  const auto named_plan = plan_native_workspace_archive_restore(f.store, primary, id, "named", named_directory, named);
  restore_native_workspace_archive(f.store, primary, id, "named", named_directory, named_plan["plan"].get<std::string>(), named);
  f.store.use_native_workspace("named");
  require(f.store.native_head()["branch"] == "recovered-branch" &&
              f.store.native_head()["revision"] == head && f.store.refs().at("heads/feature").id == other &&
              text(read_bytes(named_directory / "Source/code")) == "last observer",
      "named workspace restore moved another branch or lost archived dirty bytes");
}
void stale_index(const fs::path &base) {
  const auto primary = base / "primary", edit = base / "edit";
  Fixture f(primary);
  const auto first = f.revision(Json{{"code", f.file("first")}});
  const auto later = f.revision(Json{{"code", f.file("later")}}, Json::array({first}));
  f.install(first); f.store.create_native_branch("feature", first);
  f.store.register_native_workspace(primary, "edit", edit);
  f.store.use_native_workspace("edit"); f.store.switch_directory(edit, "feature");
  atomic_write(edit / "code", bytes("prepared"), true);
  const auto index = f.store.stage_directory(edit, "feature", {"code"});
  f.store.set_ref("heads/feature", {Kind::revision, later});
  atomic_write(edit / "code", bytes("dirty"), true);
  const auto plan = plan_native_workspace_unregistration(f.store, false, true);
  const auto result = unregister_native_workspace(f.store, plan["plan"].get<std::string>());
  const auto id = result["archive"].get<std::string>();
  NativeWorkspaceRestoreOptions settings; settings.detached = true;
  const auto destination = base / "restored";
  const auto restore = plan_native_workspace_archive_restore(f.store, primary, id, "restored", destination, settings);
  restore_native_workspace_archive(f.store, primary, id, "restored", destination, restore["plan"].get<std::string>(), settings);
  f.store.use_native_workspace("restored");
  const auto restored = f.store.refs().at(native_staging_reference(f.store, detached_native_target));
  const auto saved = f.store.metadata(Kind::revision, index), actual = f.store.metadata(Kind::revision, restored.id);
  require(actual["parents"] == saved["parents"] && actual["files"] == saved["files"] &&
              actual["parents"] == Json::array({first}) && f.store.native_head()["revision"] == later &&
              text(read_bytes(destination / "code")) == "dirty",
      "workspace restore silently rebased a stale index or confused HEAD with dirty bytes");
  f.store.gc(true); f.store.fsck();
}
void foreign_bindings(const fs::path &base) {
  const auto primary = base / "primary", edit = base / "edit";
  Fixture f(primary);
  const auto head = f.revision(Json{{"code", f.file("saved bytes")}});
  f.install(head); f.store.register_native_workspace(primary, "edit", edit);
  f.store.use_native_workspace("edit");
  f.store.detach_checkout(edit, head);
  const auto plan = plan_native_workspace_unregistration(f.store);
  const auto result = unregister_native_workspace(f.store, plan["plan"].get<std::string>());
  const auto original = read_native_workspace_archive(f.store, f.store.refs(), result["archive"].get<std::string>());
  const auto rewrite = [&](const std::string &directory) {
    auto archive = original;
    for (const auto &slot : {"registration", "workspace/HEAD"}) {
      auto &key = archive.references.at(native_context_reference("edit", slot));
      auto value = parse_json(f.store.get(key)); value["directory"] = directory;
      key = f.store.put(Kind::chunk, bytes(value.dump())).first;
    }
    archive.id = f.store.put(Kind::chunk, bytes(archive.json().dump())).first.id;
    for (const auto &[name, key] : archive.roots()) f.store.set_ref(name, key);
    return archive.id;
  };
  unsigned count = 0;
  for (const auto &directory : {"/saved/project-edit", "Q:/saved/project-edit", "//storage/share/project-edit", "/saved/name\\with-backslash"}) {
    const auto id = rewrite(directory);
    require(native_workspace_archive(f.store, id, true)["revision"] == head, "foreign historical binding was treated as a native path");
    const auto name = "foreign-" + std::to_string(++count);
    const auto destination = base / name;
    NativeWorkspaceRestoreOptions settings; settings.detached = true;
    const auto restore = plan_native_workspace_archive_restore(f.store, primary, id, name, destination, settings);
    restore_native_workspace_archive(f.store, primary, id, name, destination, restore["plan"].get<std::string>(), settings);
    require(text(read_bytes(destination / "code")) == "saved bytes", "foreign binding changed the restoration destination");
  }
  for (const auto &directory : {"relative", "Q:relative", "/saved/../escape", "Q:/saved/./edit", "//storage", "/saved//edit"}) {
    const auto id = rewrite(directory);
    fails([&] { native_workspace_archive(f.store, id); });
  }
}
}
int main(int argc, char **argv) {
  Temporary temporary;
  try {
    fixture_test::TestCases cases(fixture_test::CaseSelection::parse(argc,argv));
    for (unsigned physical = 0; physical < 3; ++physical)
      for (bool encrypted : {false,true})
        for (bool partial : {false,true})
          for (bool detached : {false,true}) {
            const auto name = "archive-"+std::to_string(physical)+'-'+std::to_string(encrypted)+'-'+
                std::to_string(partial)+'-'+std::to_string(detached);
            cases.add(name,[physical,encrypted,partial,detached](const fs::path &root) {
              std::optional<Key> key;
              if(encrypted){key.emplace();key->fill(81);}
              scenario(root,key,physical,partial,detached);
            });
          }
    cases.add("refusals",[](const fs::path &root){refusals(root);});
    cases.add("stale-index",[](const fs::path &root){stale_index(root);});
    cases.add("foreign-bindings",[](const fs::path &root){foreign_bindings(root);});
    return cases.run(temporary.path);
  } catch (const std::exception &error) {std::cerr<<error.what()<<'\n';return 1;}
}
