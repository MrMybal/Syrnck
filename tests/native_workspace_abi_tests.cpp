#include "native_history_fixture.hpp"
#include <syrnck.h>
#include <iostream>
using namespace native_test;
namespace {
void success(int code) { require(code == 0, syrnck_last_error()); }
template<class Call> Json result(Call call, bool ok = true) {
  char *out = reinterpret_cast<char *>(std::uintptr_t{1}); std::size_t length = 99;
  const auto code = call(&out, &length);
  if (!ok) { require(code != 0 && !out && !length, "native ABI failure did not reset outputs"); return {}; }
  success(code); require(out && length && out[length - 1] == 0, "invalid native ABI JSON buffer");
  const auto value = parse_json(bytes(std::string_view(out, length - 1)));
  syrnck_string_free(out); return value;
}
struct Handles {
  SyrnckStore *store = nullptr; SyrnckWorkspace *workspace = nullptr;
  Handles(const fs::path &primary, const fs::path &directory, const std::optional<Key> &key, std::string_view name) {
    success(syrnck_store_open(utf8(primary / ".syrnck").c_str(), 0,
        key ? key->data() : nullptr, key ? key->size() : 0, &store));
    success(syrnck_store_use_workspace(store, std::string(name).c_str()));
    const SyrnckWorkspaceOptions options{1, 4096, 65536};
    success(syrnck_workspace_open_registered_ex(utf8(directory).c_str(), utf8(primary / ".syrnck").c_str(),
        std::string(name).c_str(), &options, nullptr, &workspace));
  }
  ~Handles() { syrnck_workspace_close(workspace); syrnck_store_close(store); }
  Json state() { return result([&](char **out, std::size_t *n) {
    return syrnck_workspace_native_state_json_ex(workspace, store, nullptr, out, n); }); }
};
struct Cancellation {
  SyrnckCancellation *value = nullptr;
  bool seen = false;
  Cancellation() { success(syrnck_cancellation_create(&value)); }
  ~Cancellation() { syrnck_cancellation_close(value); }
  static void progress(const SyrnckProgress *event, void *user) {
    auto &state = *static_cast<Cancellation *>(user);
    if (event->phase == SYRNCK_PHASE_RECONSTRUCTING && event->bytes > 0) {
      state.seen = true; success(syrnck_cancellation_request(state.value));
    }
  }
};
void scenario(const fs::path &root, const std::optional<Key> &key, unsigned layout, unsigned scope) {
  const auto primary = root / "primary", directory = scope ? root / "edit" : primary;
  const auto context = scope ? "edit" : "";
  std::string old, target;
  const std::string incoming(2 * 1024 * 1024 + 17, 'r'), owner(64, 'a');
  {
    Fixture fixture(primary, key);
    const auto files = Json{{".syrnckignore", fixture.file("*.keep\n")},
        {"Source/item", fixture.file("before")}, {"Hidden/data", fixture.file("hidden before")}};
    old = fixture.revision(files); fixture.install(old);
    auto next = files; next.erase("Source/item");
    // Exercise streamed/cancellable reconstruction without thousands of
    // repeated 1 KiB descriptors in every platform/configuration scenario.
    next["Source/item/leaf"] = fixture.file(incoming, false, 64 * 1024);
    next["Hidden/data"] = fixture.file("hidden after");
    target = fixture.revision(next, Json::array({old}));
    Key seed; seed.fill(75);
    fixture.store.use_native_identity({"Native SDK author", "native-sdk@example.invalid", 0}, SigningKey::from_seed(seed));
    target = fixture.store.put(Kind::revision,
        bytes(fixture.store.native_commit_metadata(fixture.store.metadata(Kind::revision, target)).dump())).first.id;
    fixture.store.set_ref("incoming", {Kind::revision, target});
    if (scope) {
      if (scope == 2) fixture.store.register_partial_native_workspace(primary, "edit", directory, {"Source"});
      else fixture.store.register_native_workspace(primary, "edit", directory);
      if (scope != 3) fixture.store.create_native_branch("edit", old);
      fixture.store.use_native_workspace("edit");
      if (scope == 3) fixture.store.detach_checkout(directory, old);
      else fixture.store.switch_directory(directory, "edit");
    }
    atomic_write(directory / "unrelated.keep", bytes("ignored bytes"), false);
    fixture.pack(layout);
  }
  {
    Handles handles(primary, directory, key, context);
    const auto initial = handles.state();
    require(initial["workspace"] == context && initial["revision"]["id"] == old && !initial["staging"].get<bool>(),
        "native ABI context state differs");
    const auto selected = initial["target"].get<std::string>();
    const auto materialization = initial["materialization"]["object"].is_null() ? std::optional<std::string>{} :
        std::optional{initial["materialization"]["object"].get<std::string>()};
    const SyrnckNativeExpected expected{selected.c_str(), old.c_str(), materialization ? materialization->c_str() : nullptr};
    const auto refs_before = read_bytes(primary / ".syrnck/refs.json");
    const std::string other_policy(64, '0');
    const SyrnckNativeExpected wrong_policy{selected.c_str(), old.c_str(), other_policy.c_str()};
    result([&](char **out, std::size_t *n) { return syrnck_workspace_checkout_plan_json_ex(
        handles.workspace, handles.store, &wrong_policy, target.c_str(), nullptr, out, n); }, false);
    result([&](char **out, std::size_t *n) { return syrnck_workspace_capture_json_ex(
        handles.workspace, handles.store, nullptr, "missing expectation", nullptr, out, n); }, false);
    require(read_bytes(primary / ".syrnck/refs.json") == refs_before, "invalid native expectation changed refs");
    const auto plan = result([&](char **out, std::size_t *n) { return syrnck_workspace_checkout_plan_json_ex(
        handles.workspace, handles.store, &expected, target.c_str(), nullptr, out, n); });
    require(plan["ready"].get<bool>() && read_bytes(primary / ".syrnck/refs.json") == refs_before,
        "native ABI plan changed references or blocked clean transition");
    atomic_write(directory / "Source/item", bytes("captured local work"), true);
    auto blocked = result([&](char **out, std::size_t *n) { return syrnck_workspace_checkout_plan_json_ex(
        handles.workspace, handles.store, &expected, target.c_str(), nullptr, out, n); });
    require(!blocked["ready"].get<bool>(), "native ABI plan hid dirty work");
    result([&](char **out, std::size_t *n) { return syrnck_workspace_checkout_owned_json_ex(
        handles.workspace, handles.store, &expected, target.c_str(), owner.c_str(), nullptr, out, n); }, false);
    require(text(read_bytes(directory / "Source/item")) == "captured local work", "failed ABI checkout replaced dirty bytes");
    const auto capture = result([&](char **out, std::size_t *n) { return syrnck_workspace_capture_json_ex(
        handles.workspace, handles.store, &expected, "SDK local snapshot", nullptr, out, n); });
    const auto captured = capture["revision"].get<std::string>();
    require(captured != old && handles.state()["revision"]["id"] == captured, "native capture lost snapshot");
    const auto captured_refs = read_bytes(primary / ".syrnck/refs.json");
    result([&](char **out, std::size_t *n) { return syrnck_workspace_checkout_owned_json_ex(
        handles.workspace, handles.store, &expected, target.c_str(), owner.c_str(), nullptr, out, n); }, false);
    require(read_bytes(primary / ".syrnck/refs.json") == captured_refs, "stale ABI expectation changed refs");
    const SyrnckNativeExpected current{selected.c_str(), captured.c_str(), materialization ? materialization->c_str() : nullptr};
    Cancellation stop;
    const SyrnckOperation operation{stop.value, Cancellation::progress, &stop};
    result([&](char **out, std::size_t *n) { return syrnck_workspace_checkout_owned_json_ex(
        handles.workspace, handles.store, &current, target.c_str(), owner.c_str(), &operation, out, n); }, false);
    const auto pending = handles.state();
    require(stop.seen && pending["checkout"]["pending"].get<bool>() && pending["checkout"]["owner"] == owner,
        "native ABI cancellation did not retain owned journal");
    const auto stopped_refs = read_bytes(primary / ".syrnck/refs.json");
    for (const auto &wrong : {old, std::string(64, 'b')}) {
      result([&](char **out, std::size_t *n) { return syrnck_workspace_recover_owned_json_ex(
          handles.workspace, handles.store, selected.c_str(), target.c_str(), wrong.c_str(), nullptr, out, n); }, false);
    }
    result([&](char **out, std::size_t *n) { return syrnck_workspace_recover_owned_json_ex(
        handles.workspace, handles.store, selected.c_str(), old.c_str(), owner.c_str(), nullptr, out, n); }, false);
    require(read_bytes(primary / ".syrnck/refs.json") == stopped_refs, "foreign ABI recovery changed refs");
  }
  // Recover with brand-new ABI handles, without the cancelled source/session.
  {
    Handles handles(primary, directory, key, context);
    const auto pending = handles.state(); const auto selected = pending["target"].get<std::string>();
    result([&](char **out, std::size_t *n) { return syrnck_workspace_recover_owned_json_ex(
        handles.workspace, handles.store, selected.c_str(), target.c_str(), owner.c_str(), nullptr, out, n); });
    const auto transaction = pending["checkout"]["id"].get<std::string>();
    const auto receipt = result([&](char **out, std::size_t *n) { return syrnck_workspace_checkout_status_json_ex(
        handles.workspace, handles.store, transaction.c_str(), nullptr, out, n); });
    require(!receipt["pending"].get<bool>() && receipt["state"] == "committed" && receipt["owner"] == owner &&
        receipt["target"] == target && receipt["context"] == context, "native ABI receipt lost completion evidence");
    result([&](char **out, std::size_t *n) { return syrnck_workspace_checkout_status_json_ex(
        handles.workspace, handles.store, nullptr, nullptr, out, n); }, false);
    const std::string absent(64, 'e');
    result([&](char **out, std::size_t *n) { return syrnck_workspace_checkout_status_json_ex(
        handles.workspace, handles.store, absent.c_str(), nullptr, out, n); }, false);
    require(handles.state()["revision"]["id"] == target && !handles.state()["checkout"]["pending"].get<bool>(),
        "native ABI recovery did not finish exact candidate");
    require(text(read_bytes(directory / "Source/item/leaf")) == incoming &&
        text(read_bytes(directory / "unrelated.keep")) == "ignored bytes", "native ABI recovery lost working data");
    require(scope != 2 || !fs::exists(directory / "Hidden"), "partial ABI checkout materialized omitted data");
    const auto status = result([&](char **out, std::size_t *n) { return syrnck_workspace_status_json_ex(
        handles.workspace, handles.store, nullptr, 0, nullptr, out, n); });
    require(status["clean"].get<bool>() && status["observation"]["full_scan"].get<bool>(),
        "native mutation reused stale working observation");
    result([&](char **out, std::size_t *n) { return syrnck_workspace_recover_owned_json_ex(
        handles.workspace, handles.store, selected.c_str(), target.c_str(), owner.c_str(), nullptr, out, n); }, false);
  }
  {
    Store store(primary / ".syrnck", key); store.use_native_workspace(context);
    (void)store.gc(true); (void)store.fsck();
    require(store.metadata(Kind::revision, target)["files"].contains("Hidden/data"), "partial ABI checkout lost full tree");
  }
}
void preserves_edit_during_application(const fs::path &root) {
  std::string original, candidate;
  {
    Fixture fixture(root);
    original = fixture.revision(Json{{"asset", fixture.file("source")}}); fixture.install(original);
    candidate = fixture.revision(Json{{"asset", fixture.file("incoming")}}, Json::array({original}));
    fixture.store.set_ref("incoming", {Kind::revision, candidate});
  }
  Handles handles(root, root, {}, "");
  const auto initial = handles.state(); const auto target = initial["target"].get<std::string>();
  const SyrnckNativeExpected expected{target.c_str(), original.c_str(), nullptr};
  struct Edit {
    fs::path path;
    bool changed = false, failed = false;
    static void progress(const SyrnckProgress *event, void *user) {
      auto &edit = *static_cast<Edit *>(user);
      if (event->phase == SYRNCK_PHASE_PUBLISHING && !edit.changed) {
        edit.changed = true;
        try { atomic_write(edit.path, bytes("user edit during application"), true); }
        catch (...) { edit.failed = true; }
      }
    }
  } edit{root / "asset"};
  const std::string owner(64, 'c'); const SyrnckOperation operation{nullptr, Edit::progress, &edit};
  result([&](char **out, std::size_t *n) { return syrnck_workspace_checkout_owned_json_ex(
      handles.workspace, handles.store, &expected, candidate.c_str(), owner.c_str(), &operation, out, n); }, false);
  const auto state = handles.state();
  require(edit.changed && !edit.failed && state["revision"]["id"] == original && state["checkout"]["pending"].get<bool>() &&
      state["checkout"]["owner"] == owner && text(read_bytes(root / "asset")) == "user edit during application",
      "active owned application overwrote or acknowledged concurrent local work");
  result([&](char **out, std::size_t *n) { return syrnck_workspace_recover_owned_json_ex(
      handles.workspace, handles.store, target.c_str(), candidate.c_str(), owner.c_str(), nullptr, out, n); }, false);
  require(text(read_bytes(root / "asset")) == "user edit during application" && handles.state()["checkout"]["pending"].get<bool>(),
      "owned retry overwrote concurrent local work");
}

}
int main() {
  try {
    Temporary temp;
    for (bool encrypted : {false, true}) for (unsigned layout = 0; layout != 3; ++layout)
      for (unsigned scope = 0; scope != 4; ++scope) {
        std::optional<Key> key; if (encrypted) { key.emplace(); key->fill(73); }
        scenario(temp.path / (std::to_string(encrypted) + '-' + std::to_string(layout) + '-' + std::to_string(scope)), key, layout, scope);
      }
    preserves_edit_during_application(temp.path / "active-edit");
    std::cout << "24 native ABI capture/owned recovery scenarios and active-edit preservation passed\n"; return 0;
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
