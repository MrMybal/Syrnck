#include "native_history_fixture.hpp"
#include "native_directory_identity.hpp"
#include <syrnck/workspace.hpp>
#include <syrnck/backup.hpp>
#include <iostream>
using namespace native_test;
namespace {
std::map<std::string, std::string> files(const fs::path &directory) {
  std::map<std::string, std::string> result;
  for (const auto &entry : fs::recursive_directory_iterator(directory))
    if (!entry.is_symlink() && entry.is_regular_file())
      result.emplace(utf8(entry.path().lexically_relative(directory)), text(read_bytes(entry.path())));
  return result;
}
void scenario(const fs::path &base, std::optional<Key> key, unsigned physical, bool partial, bool detached) {
  Fixture f(base / "primary", key);
  const auto source = base / "edit", destination = base / "moved";
  const auto head = f.revision(Json{{"Source/code", f.file("base")}, {"Content/asset", f.file("complete hidden history")}});
  f.install(head); f.store.create_native_branch("feature", head);
  if (partial) f.store.register_partial_native_workspace(base / "primary", "edit", source, {"Source"});
  else f.store.register_native_workspace(base / "primary", "edit", source);
  f.store.use_native_workspace("edit");
  if (detached) f.store.detach_checkout(source, head); else f.store.switch_directory(source, "feature");
  atomic_write(source / "Source/code", bytes("prepared"), true);
  const auto index = f.store.stage_directory(source, f.store.native_target(), {"Source"});
  atomic_write(source / "Source/code", bytes("dirty"), true);
  atomic_write(source / "ignored.tmp", bytes("unversioned bytes"), false);
  fs::create_directories(source / ".git"); atomic_write(source / ".git/opaque", bytes("preserve opaque metadata"), false);
  fs::create_directory(source / "empty");
  const auto external = base / "external"; fs::create_directory(external);
  atomic_write(external / "keep", bytes("external link target"), false);
  std::error_code link_error; fs::create_directory_symlink(external, source / "link", link_error);
  const auto linked = !link_error;
  const auto original = files(source);
  const auto directory_id = native_directory_identity(source);
  f.store.use_native_workspace(""); f.pack(physical);
  const auto storage = f.physical();
  const auto refs = f.store.refs();
  const auto plan = plan_native_workspace_relocation(f.store, "edit", destination);
  require(plan["ready"] == true && f.physical() == storage && files(source) == original,
      "relocation preview wrote data or scanned protected descendants");
  const auto result = relocate_native_workspace(f.store, "edit", destination, plan["plan"].get<std::string>());
  require(result["state"] == "completed" && !fs::exists(source) && files(destination) == original &&
      native_directory_identity(destination) == directory_id && fs::is_directory(destination / "empty"),
      "whole workspace relocation lost files, empty directories or physical identity");
  const auto completed = f.physical();
  relocate_native_workspace(f.store, "edit", destination, plan["plan"].get<std::string>());
  require(f.physical() == completed, "completed relocation retry rewrote the Store");
  if (linked) require(fs::is_symlink(fs::symlink_status(destination / "link")) &&
      text(read_bytes(external / "keep")) == "external link target", "relocation followed or lost a descendant symlink");
  require(native_workspace_relocation_status(f.store)["pending"] == false &&
      f.store.refs().at("heads/main") == refs.at("heads/main") && f.store.refs().at("heads/feature") == refs.at("heads/feature"),
      "relocation changed shared revision history");
  f.store.use_native_workspace("edit");
  require(f.store.native_head()["revision"] == head, "relocation changed HEAD revision");
  require(f.store.native_head()["directory"] == utf8(fs::canonical(destination)), "relocation HEAD directory mismatch");
  require(f.store.native_head()["branch"].is_null() == detached, "relocation changed named/detached selection");
  require(f.store.refs().at(native_staging_reference(f.store, f.store.native_target())).id == index, "relocation changed frozen index");
  (void)f.store.checkout_status();
  f.store.gc(true); f.store.fsck();
  f.store.export_revision(head, base / "complete");
  require(text(read_bytes(base / "complete/Content/asset")) == "complete hidden history", "relocation lost excluded history");
  f.store.use_native_workspace("");
  fs::create_directory(source); atomic_write(source / "foreign", bytes("new owner"), false);
  const auto collected = f.physical();
  relocate_native_workspace(f.store, "edit", destination, plan["plan"].get<std::string>());
  require(f.physical() == collected && text(read_bytes(source / "foreign")) == "new owner",
      "completed relocation retry after GC touched a reused source path");
}
void recovery(const fs::path &base, unsigned pause, bool abort, bool case_only) {
  Fixture f(base / "primary");
  const auto head = f.revision(Json{{"code", f.file("base")}});
  f.install(head);
  const auto source = base / "edit", destination = base / (case_only ? "EDIT" : "moved");
  f.store.register_native_workspace(base / "primary", "edit", source);
  f.store.use_native_workspace("edit"); f.store.detach_checkout(source, head); f.store.use_native_workspace("");
  atomic_write(source / "local", bytes("keep me"), false);
  const auto original = files(source);
  const auto plan = plan_native_workspace_relocation(f.store, "edit", destination);
  std::stop_source stop; unsigned moves = 0;
  fails([&] { relocate_native_workspace(f.store, "edit", destination, plan["plan"].get<std::string>(),
      {stop.get_token(), [&](const ProgressEvent &event) {
        if (event.path == "workspace-relocation/move" && ++moves == pause) stop.request_stop();
        if (event.path == "workspace-relocation/finalize" && pause == 3) stop.request_stop();
      }}); });
  const auto status = native_workspace_relocation_status(f.store);
  require(status["pending"] == true && status["state"] == (pause == 1 ? "source" : pause == 2 ? "bridge" : "destination"),
      "relocation cancellation did not retain its physical phase");
  const auto refs = f.store.refs();
  fails([&] { f.store.gc(true); });
  fails([&] { create_backup(f.store, base / "blocked-backup"); });
  fails([&] { f.store.set_ref("unrelated", {Kind::revision, head}); });
  require(f.store.refs() == refs, "pending relocation allowed an unrelated reference publication");
  const auto result = recover_native_workspace_relocation(f.store, abort);
  const auto location = abort ? source : destination;
  require(result["state"] == (abort ? "aborted" : "completed") && files(location) == original &&
      native_workspace_relocation_status(f.store)["pending"] == false, "relocation resume or abort lost working bytes");
  f.store.use_native_workspace("edit");
  require(f.store.native_head()["directory"] == utf8(fs::canonical(location)) && f.store.native_head()["revision"] == head,
      "relocation recovery published the wrong binding");
  f.store.gc(true); f.store.fsck();
}
void refusals(const fs::path &base) {
  Fixture f(base / "primary");
  const auto head = f.revision(Json{{"code", f.file("base")}});
  f.install(head);
  const auto source = base / "edit", destination = base / "moved";
  f.store.register_native_workspace(base / "primary", "edit", source);
  f.store.use_native_workspace("edit"); f.store.detach_checkout(source, head); f.store.use_native_workspace("");
  const auto refs = f.store.refs();
  auto registration = parse_json(f.store.get(refs.at("workspaces/edit/registration")));
  auto selection = parse_json(f.store.get(refs.at("workspaces/edit/workspace/HEAD")));
  registration["directory"] = utf8(fs::canonical(base) / "moved"); selection["directory"] = registration["directory"];
  const auto changed_registration = f.store.put(Kind::chunk, bytes(registration.dump())).first;
  const auto changed_head = f.store.put(Kind::chunk, bytes(selection.dump())).first;
  fails([&] { f.store.compare_update_refs({{"workspaces/edit/registration", refs.at("workspaces/edit/registration"), changed_registration},
      {"workspaces/edit/workspace/HEAD", refs.at("workspaces/edit/workspace/HEAD"), changed_head}}); });
  require(f.store.refs() == refs && fs::exists(source / "code"), "raw binding rewrite bypassed relocation recovery");
  fs::create_directory(destination); atomic_write(destination / "foreign", bytes("keep"), false);
  fails([&] { plan_native_workspace_relocation(f.store, "edit", destination); });
  require(text(read_bytes(destination / "foreign")) == "keep", "occupied relocation destination was changed");
  fs::remove(destination / "foreign"); fs::remove(destination);
  const auto plan = plan_native_workspace_relocation(f.store, "edit", destination);
  bool changed = false;
  fails([&] { relocate_native_workspace(f.store, "edit", destination, plan["plan"].get<std::string>(),
      {{}, [&](const ProgressEvent &event) {
        if (!changed && event.path == "workspace-relocation/prepare") {
          changed = true; fs::create_directory(destination); atomic_write(destination / "foreign", bytes("callback"), false);
        }
      }}); });
  require(changed && native_workspace_relocation_status(f.store)["pending"] == false &&
      fs::exists(source / "code") && text(read_bytes(destination / "foreign")) == "callback",
      "relocation ignored a destination created by its publishing observer");
  fs::remove(destination / "foreign"); fs::remove(destination);
  const auto parent = base / "new-parent";
  fs::create_directory(parent);
  const auto parent_plan = plan_native_workspace_relocation(f.store, "edit", parent / "moved");
  fs::rename(parent, base / "old-parent"); fs::create_directory(parent);
  fails([&] { relocate_native_workspace(f.store, "edit", parent / "moved", parent_plan["plan"].get<std::string>()); });
  require(fs::exists(source / "code") && native_workspace_relocation_status(f.store)["pending"] == false,
      "relocation ignored a replaced destination parent");
  const auto next = plan_native_workspace_relocation(f.store, "edit", destination);
  relocate_native_workspace(f.store, "edit", destination, next["plan"].get<std::string>(),
      {{}, [&](const ProgressEvent &event) {
        if (event.path == "workspace-relocation/prepare") atomic_write(source / "code", bytes("latest bytes"), true);
      }});
  require(text(read_bytes(destination / "code")) == "latest bytes", "identity-preserving relocation lost a descendant edit");
}
}
int main() {
  Temporary temporary;
  try {
    for (unsigned physical = 0; physical < 3; ++physical)
      for (bool encrypted : {false, true}) for (bool partial : {false, true}) for (bool detached : {false, true}) {
        std::optional<Key> key; if (encrypted) { key.emplace(); key->fill(84); }
        scenario(temporary.path / (std::to_string(physical) + '-' + std::to_string(encrypted) + '-' +
            std::to_string(partial) + '-' + std::to_string(detached)), key, physical, partial, detached);
      }
    for (unsigned pause : {1U, 2U, 3U}) for (bool abort : {false, true}) for (bool case_only : {false, true})
      recovery(temporary.path / ("recovery-" + std::to_string(pause) + '-' + std::to_string(abort) + '-' + std::to_string(case_only)),
          pause, abort, case_only);
    refusals(temporary.path / "refusals");
    std::cout << "Whole workspace moves preserve identity, local files and recoverable bindings\n";
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
