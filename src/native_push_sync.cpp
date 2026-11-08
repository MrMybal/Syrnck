#include "native_context.hpp"
#include "native_workspace.hpp"
#include <syrnck/native_sync.hpp>
namespace syrnck::net {
namespace {
std::optional<ObjectKey> lookup(const Refs &refs, const std::string &name) {
  const auto at = refs.find(name);
  return at == refs.end() ? std::optional<ObjectKey>{} : std::optional{at->second};
}
Json identifier(const std::optional<ObjectKey> &key) { return key ? Json(key->id) : Json{}; }
struct Checkpoint {
  std::optional<ObjectKey> descriptor, baseline;
};
Checkpoint read_checkpoint(const Store &store, const Refs &refs, const std::string &state_ref,
                          const std::string &base_ref, const Json &binding) {
  Checkpoint result{lookup(refs, state_ref), lookup(refs, base_ref)};
  require(result.descriptor.has_value() == result.baseline.has_value(), "invalid native push checkpoint");
  if (!result.descriptor) return result;
  require(result.descriptor->kind == Kind::chunk && result.baseline->kind == Kind::revision, "invalid native push checkpoint");
  auto expected = binding; expected["revision"] = result.baseline->id;
  require(text(store.get(*result.descriptor)) == expected.dump(), "invalid native push checkpoint");
  (void)store.metadata(Kind::revision, result.baseline->id);
  return result;
}
void acknowledge(const Store &store, const Checkpoint &old, const std::string &state_ref,
                 const std::string &base_ref, const Json &binding, const ObjectKey &head,
                 std::string_view head_ref, std::string_view service, OperationOptions options) {
  if (old.baseline == head) return;
  const auto refs = store.refs();
  require(lookup(refs, state_ref) == old.descriptor && lookup(refs, base_ref) == old.baseline,
      "native push checkpoint changed; retry synchronization");
  std::vector<RefUpdate> updates{{base_ref, old.baseline, head}, {std::string(head_ref), head, head}};
  if (old.baseline) {
    const auto archive = "archive/agent-push/" + std::string(service) + '/' + old.baseline->id;
    const auto at = lookup(refs, archive);
    require(!at || *at == *old.baseline, "native push archive reference is occupied");
    updates.push_back({archive, at, old.baseline});
  }
  auto descriptor = binding; descriptor["revision"] = head.id;
  const auto key = store.put(Kind::chunk, bytes(descriptor.dump())).first;
  updates.push_back({state_ref, old.descriptor, key});
  require(store.compare_update_refs(updates, options), "native push checkpoint changed; retry synchronization");
}
}
struct NativePushSync::Impl {
  fs::path storage, directory;
  std::string workspace, target;
  NativePushSyncOptions settings;
  WorkspaceObservation observation;
  std::optional<ObjectKey> observed_head;
  std::optional<std::string> endpoint;
  bool rescan = true;
  std::optional<bool> clean;
  Impl(const Store &store, const fs::path &path, NativePushSyncOptions options)
      : storage(fs::canonical(store.root())), directory(fs::canonical(path)), workspace(store.native_workspace_name()),
        target(store.native_target()), settings(std::move(options)), observation(directory, storage) {
    validate_ref(settings.remote_branch);
    require(settings.remote_branch.find('/') == settings.remote_branch.npos && settings.remote_branch.size() <= 64,
        "native remote names must be single components of at most 64 bytes");
    store.validate_native_workspace(directory);
  }
};
NativePushSync::NativePushSync(const Store &store, const fs::path &directory, NativePushSyncOptions settings)
    : impl_(std::make_unique<Impl>(store, directory, std::move(settings))) {}
NativePushSync::~NativePushSync() = default;
NativePushSync::NativePushSync(NativePushSync &&) noexcept = default;
NativePushSync &NativePushSync::operator=(NativePushSync &&) noexcept = default;
void NativePushSync::invalidate() { impl_->rescan = true; impl_->observation.invalidate(); }
Json NativePushSync::tick(Store &store, const Remote &remote, const WorkspaceChanges &hints,
                         OperationOptions options, TransferOptions transfer) {
  auto &state = *impl_;
  throw_if_cancelled(options.stop); transfer.validate();
  require(fs::canonical(store.root()) == state.storage && store.native_workspace_name() == state.workspace,
      "native push Store or workspace changed");
  require(store.native_target() == state.target, "native push HEAD selection changed");
  store.validate_native_workspace(state.directory);
  const auto endpoint = remote.endpoint_fingerprint();
  require(!state.endpoint || *state.endpoint == endpoint, "native push remote endpoint changed");
  state.endpoint = endpoint;
  // Logical identity survives portable primary restore and in-place relocation;
  // physical destinations always come from the caller and the live binding.
  const Json binding{{"version", 1}, {"type", "native-push-sync"}, {"workspace", state.workspace},
      {"target", state.target}, {"remote_branch", state.settings.remote_branch}, {"endpoint", endpoint}};
  const auto service = hash(bytes(binding.dump()));
  const auto state_ref = store.native_workspace_reference("sync/push/" + service + "/state");
  const auto base_ref = store.native_workspace_reference("sync/push/" + service + "/baseline");
  const auto head_ref = native_revision_reference(store, state.target);
  const auto refs = store.refs();
  const auto saved = read_checkpoint(store, refs, state_ref, base_ref, binding);
  auto head = lookup(refs, head_ref);
  require(!head || head->kind == Kind::revision, "branch must point to a revision");
  Json result{{"version", 1}, {"state", "idle"}, {"reason", Json{}}, {"service", service},
      {"local_revision", identifier(head)}, {"remote_revision", Json{}}, {"baseline", identifier(saved.baseline)},
      {"working_clean", state.clean ? Json(*state.clean) : Json{}}, {"observed", false}, {"full_scan", false},
      {"committed", false}, {"transfer", Json{}}};
  const auto blocked = [&](std::string_view reason) {
    result["state"] = "blocked"; result["reason"] = reason; return result;
  };
  if (store.checkout_status()["pending"].get<bool>()) return blocked("checkout_pending");
  require_no_native_checkout(store);
  if (refs.contains(native_staging_reference(store, state.target))) return blocked("staging_in_progress");
  if (state.rescan || hints.changed() || state.observed_head != head) {
    auto changes = hints; changes.full_scan |= state.rescan;
    const auto status = state.observation.status(store, head ? std::optional{head->id} : std::nullopt, changes, options);
    state.clean = status["clean"].get<bool>(); state.observed_head = head; state.rescan = false;
    result["observed"] = true; result["working_clean"] = *state.clean;
    result["full_scan"] = status["observation"]["full_scan"];
    if (!*state.clean && state.settings.capture_changes) {
      auto ingest = store.settings().ingest_options(); ingest.stop = options.stop; ingest.progress = options.progress;
      head = ObjectKey{Kind::revision, store.commit_directory(state.directory, state.target,
          "Syrnck automatic workspace snapshot", ingest)};
      result["committed"] = true; result["local_revision"] = head->id;
      state.clean.reset(); result["working_clean"] = Json{};
      state.observed_head = head; state.observation.invalidate(); state.rescan = true;
    }
  }
  auto push_limits = transfer; push_limits.require_read_lease = false;
  // A new local root must not bypass an unknown publication of a previous root.
  const auto sessions = store.transfer_sessions(options.stop);
  require(sessions.size() <= 65536, "native push transfer session limit exceeded");
  for (const auto &session : sessions) {
    throw_if_cancelled(options.stop);
    if (session.direction == "push" && session.endpoint == endpoint && session.reference == state.settings.remote_branch &&
        (session.state == "publishing" || session.state == "uncertain"))
      (void)remote.resume(store, session.id, options, push_limits);
  }
  const auto incoming = remote.refs(options, transfer);
  auto observed = lookup(incoming, state.settings.remote_branch);
  require(!observed || observed->kind == Kind::revision, "native push remote reference is not a revision");
  result["remote_revision"] = identifier(observed);
  if (head && observed == head) {
    acknowledge(store, saved, state_ref, base_ref, binding, *head, head_ref, service, options);
    result["baseline"] = head->id;
    result["state"] = state.clean && !*state.clean ? "uncommitted" : "synced";
    return result;
  }
  if (!head) { result["state"] = observed ? "blocked" : "empty"; result["reason"] = observed ? Json("remote_ahead") : Json{}; return result; }
  if (saved.baseline && !observed) return blocked("remote_deleted");
  if (saved.baseline && !store.native_ancestry(saved.baseline->id, head->id, {}, options)["source_is_ancestor"].get<bool>())
    return blocked("local_history_rewritten");
  if (observed) {
    const auto tracking_name = "agent-" + service.substr(0, 32);
    const auto tracking = lookup(store.refs(), "remotes/" + tracking_name + '/' + state.settings.remote_branch);
    if (tracking != observed && observed != saved.baseline) {
      auto pull_limits = transfer; pull_limits.require_upload_lease = false; pull_limits.asset_locks.clear();
      const auto fetched = fetch_native_branch(store, remote, state.settings.remote_branch, tracking_name, options, pull_limits);
      observed = ObjectKey{Kind::revision, fetched["revision"].get<std::string>()};
      result["remote_revision"] = observed->id;
    }
    if (observed == head) {
      acknowledge(store, saved, state_ref, base_ref, binding, *head, head_ref, service, options);
      result["state"] = state.clean && !*state.clean ? "uncommitted" : "synced";
      result["baseline"] = head->id;
      return result;
    }
    const auto graph = store.native_ancestry(observed->id, head->id, {}, options);
    if (!graph["source_is_ancestor"].get<bool>())
      return blocked(graph["target_is_ancestor"].get<bool>() ? "remote_ahead" : "diverged");
    if (saved.baseline && observed != saved.baseline &&
        store.native_ancestry(observed->id, saved.baseline->id, {}, options)["source_is_ancestor"].get<bool>())
      return blocked("remote_history_rewritten");
  }
  const auto session_id = TransferSession::identity("push", endpoint, *head, state.settings.remote_branch);
  if (const auto old = store.transfer_session(session_id); old && old->state == "conflict")
    return blocked("transfer_conflict");
  result["transfer"] = remote.push_expected(store, *head, state.settings.remote_branch, observed, options, push_limits);
  acknowledge(store, saved, state_ref, base_ref, binding, *head, head_ref, service, options);
  result["state"] = "pushed";
  result["baseline"] = head->id;
  result["expected_remote_revision"] = identifier(observed);
  result["remote_revision"] = head->id;
  return result;
}
} // namespace syrnck::net
