#include "native_history_fixture.hpp"
#include "native_workspace.hpp"
#include <syrnck/backup.hpp>
#include <iostream>
using namespace native_test;
namespace {
const std::string owner(64, 'a'), other(64, 'b');
OperationOptions pause() {
  return {{}, [](const ProgressEvent &event) {
    if (event.phase == ProgressPhase::exporting) throw Cancelled("test interrupted owned checkout");
  }};
}
void scenario(const fs::path &root, std::optional<Key> key, std::string scope, bool transition) {
  Fixture f(root / "primary", key);
  const bool partial = scope == "partial" || scope == "partial-detached";
  const bool detached = scope == "detached" || scope == "partial-detached";
  auto files = Json{{"Content/hidden", f.file("hidden before")}, {"Source/item", f.file("before")}};
  const auto initial = f.revision(files);
  f.install(initial);
  files["Content/hidden"] = f.file("hidden after");
  if (transition) files.erase("Source/item");
  files[transition ? "Source/item/leaf" : "Source/item"] = f.file("after");
  const auto target = f.revision(files, Json::array({initial}));
  f.store.set_ref("saved-target", {Kind::revision, target});
  const auto directory = root / (scope == "primary" ? "primary" : "edit");
  if (scope != "primary") {
    if (partial) f.store.register_partial_native_workspace(f.project, "edit", directory, {"Source"});
    else f.store.register_native_workspace(f.project, "edit", directory);
    if (!detached) f.store.create_native_branch("edit", initial);
    f.store.use_native_workspace("edit");
    if (detached) f.store.detach_checkout(directory, initial);
    else f.store.switch_directory(directory, "edit");
  }
  const auto branch = f.store.native_target();
  const auto features = f.store.required_features();
  require(f.store.checkout_directory_owned(directory, branch, initial, owner)["state"] == "unchanged" &&
      f.store.required_features() == features, "owned no-op enabled an unnecessary reader feature");
  const auto before_invalid = f.physical();
  fails([&] { f.store.checkout_directory_owned(directory, branch, target, "invalid"); });
  require(f.physical() == before_invalid, "invalid ownership wrote storage");
  fails([&] { f.store.checkout_directory_owned(directory, branch, target, owner, pause()); });
  const auto pending = f.store.checkout_status();
  require(pending["pending"] == true && pending["owner"] == owner && pending["version"] == 11 &&
      pending["source"] == initial && pending["target"] == target, "owned checkout lost its frozen identity");
  const auto protected_state = f.physical();
  fails([&] { f.store.recover_checkout_owned(directory, branch, other); });
  fails([&] { f.store.recover_checkout_owned(directory, "wrong-branch", owner); });
  const auto elsewhere = root / "elsewhere"; fs::create_directory(elsewhere);
  fails([&] { f.store.recover_checkout_owned(elsewhere, branch, owner); });
  require(f.physical() == protected_state && fs::is_empty(elsewhere), "mismatched recovery changed data");
  if (scope != "primary") {
    f.store.use_native_workspace("");
    fails([&] { f.store.recover_checkout_owned(directory, branch, owner); });
    f.store.use_native_workspace("edit");
    require(f.physical() == protected_state, "wrong-context recovery changed storage");
  }
  const auto recovered = f.store.recover_checkout_owned(directory, branch, owner);
  require(recovered["state"] == "committed" && recovered["owner"] == owner &&
      f.store.native_head()["revision"] == target && f.store.checkout_status()["pending"] == false,
      "owned recovery did not finish the exact frozen target");
  require(text(read_bytes(directory / (transition ? "Source/item/leaf" : "Source/item"))) == "after",
      "owned recovery restored incorrect bytes");
  if (partial) require(!fs::exists(directory / "Content"), "owned recovery materialized hidden content");
  if (scope != "primary") require(text(read_bytes(f.project / "Source/item")) == "before", "linked owned recovery changed primary files");
  const auto receipt_ref = f.store.native_workspace_reference("checkouts/receipts/" + pending["id"].get<std::string>());
  const auto receipt = f.store.refs().at(receipt_ref);
  const auto original = parse_json(f.store.get(receipt));
  for (unsigned mutation = 0; mutation < 5; ++mutation) {
    auto malformed = original;
    if (mutation == 0) malformed["owner"] = other;
    if (mutation == 1) malformed.erase("owner");
    if (mutation == 2) malformed["owner"] = 42;
    if (mutation == 3) malformed["layout_version"] = 5;
    if (mutation == 4) malformed["target_materialization"] = Json::array({"Different"});
    const auto forged = f.store.put(Kind::chunk, bytes(malformed.dump())).first;
    fails([&] { read_native_checkout_receipt(f.store, forged, pending["id"].get<std::string>()); });
  }
  f.store.gc(true); f.store.fsck();
  require(read_native_checkout_receipt(f.store, receipt, pending["id"].get<std::string>())["owner"] == owner,
      "GC lost owned receipt identity");
  if (scope == "primary" && !transition) {
    create_backup(f.store, root / "backup", key);
    Store backup(root / "backup", key); verify_backup(backup);
    restore_backup(backup, root / "restored", key);
    Store restored(root / "restored", key); restored.fsck();
    require(restored.required_features() == f.store.required_features() && restored.checkout_status()["pending"] == false,
        "backup dropped reader requirements or activated an old owned checkout");
    restored.export_revision(target, root / "exported");
    require(text(read_bytes(root / "exported/Source/item")) == "after", "restored owned target needed machine state");
  }
}
void manual(const fs::path &root) {
  Fixture f(root);
  const auto first = f.revision(Json{{"file", f.file("before")}}); f.install(first);
  const auto next = f.revision(Json{{"file", f.file("after")}}, Json::array({first}));
  fails([&] { f.store.checkout_directory(f.project, "main", next, pause()); });
  const auto pending = f.store.checkout_status();
  const auto before = f.physical();
  require(pending["pending"] == true && !pending.contains("owner") && pending["version"] != 11,
      "ordinary checkout unexpectedly acquired ownership");
  fails([&] { f.store.recover_checkout_owned(f.project, "main", owner); });
  require(f.physical() == before, "automatic recovery took over a manual transaction");
  f.store.recover_checkout(); f.store.fsck();
}
}
int main() {
  Temporary temporary;
  try {
    for (bool encrypted : {false, true}) for (const auto *scope : {"primary", "linked", "partial", "detached", "partial-detached"})
      for (bool transition : {false, true}) {
        std::optional<Key> key; if (encrypted) { key.emplace(); key->fill(91); }
        scenario(temporary.path / (std::to_string(encrypted) + '-' + scope + '-' + std::to_string(transition)), key, scope, transition);
      }
    manual(temporary.path / "manual");
    std::cout << "Owned checkout identity, context, namespace transitions and guarded recovery verified\n";
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
