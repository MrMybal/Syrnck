#include "native_context.hpp"
#include "native_workspace.hpp"
#include <syrnck/native_sync.hpp>
#include <syrnck/workspace.hpp>
namespace syrnck::net {
namespace {
std::optional<ObjectKey> lookup(const Refs &refs, const std::string &name) {
  const auto at = refs.find(name);
  return at == refs.end() ? std::optional<ObjectKey>{} : std::optional{at->second};
}
class PlainSyncRemote final : public NativeSyncRemote {
public:
  explicit PlainSyncRemote(const Remote &remote) : remote_(remote) {}
  std::string endpoint_fingerprint() const override { return remote_.endpoint_fingerprint(); }
  std::vector<NativeSyncSession> sessions(const Store &store, std::string_view branch,
      bool include_completed, OperationOptions options, TransferOptions transfer) override {
    (void)transfer;
    const auto all = store.transfer_sessions(options.stop);
    require(all.size() <= 65536, "native push transfer session limit exceeded");
    std::vector<NativeSyncSession> result;
    const auto endpoint = endpoint_fingerprint();
    for (const auto &session : all) {
      if (session.direction != "push" || session.endpoint != endpoint || session.reference != branch) continue;
      if (session.state == "complete" &&
          (!include_completed || session.root.kind != Kind::revision || !store.contains(session.root))) continue;
      result.push_back({session.id, session.root, session.state});
    }
    return result;
  }
  NativeSyncObservation observe(const Store &, std::string_view branch, OperationOptions options,
      TransferOptions transfer) override {
    return {lookup(remote_.refs(options, transfer), std::string(branch)), {}};
  }
  Json fetch(const Store &store, std::string_view branch, std::string_view name,
      OperationOptions options, TransferOptions transfer) override {
    auto fetched = fetch_native_branch(store, remote_, branch, name, options, transfer);
    const auto revision = store.metadata(Kind::revision, fetched["revision"].get<std::string>());
    require(revision["message"] != "Syrnck sealed snapshot v1" || revision["files"].size() != 1 ||
        !revision["files"].contains("sealed/root"), "sealed remote requires agent --sealed");
    return fetched;
  }
  Json resume(const Store &store, std::string_view session, OperationOptions options,
      TransferOptions transfer, const PublicationObserver &published) override {
    return remote_.resume(store, session, options, transfer, published);
  }
  bool conflicted(const Store &store, const ObjectKey &root, std::string_view branch) const override {
    const auto old = store.transfer_session(TransferSession::identity("push", endpoint_fingerprint(), root, branch));
    return old && old->state == "conflict";
  }
  Json push_expected(const Store &store, const ObjectKey &root, std::string_view branch,
      const std::optional<ObjectKey> &expected, OperationOptions options, TransferOptions transfer,
      const PublicationObserver &published) override {
    return remote_.push_expected(store, root, branch, expected, options, transfer, published);
  }
private:
  const Remote &remote_;
};
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
bool observe_revision(const Store &store, const std::string &reference, std::optional<ObjectKey> &observed,
                      const std::optional<ObjectKey> &baseline, const ObjectKey &root,
                      std::string_view service, bool historical, OperationOptions options) {
  require(root.kind == Kind::revision, "native push remote reference is not a revision");
  const auto refs = store.refs();
  require(lookup(refs, reference) == observed, "native remote observation changed; retry synchronization");
  const auto known = observed ? observed : baseline;
  if (known && *known != root) {
    const auto graph = store.native_ancestry(known->id, root.id, {}, options);
    if (!graph["source_is_ancestor"].get<bool>())
      return historical && graph["target_is_ancestor"].get<bool>();
  }
  if (observed == root) return true;
  store.require_features({"resident-sync-v1"}, options);
  std::vector<RefUpdate> updates{{reference, observed, root}};
  if (observed) {
    const auto archive = "archive/agent-observation/" + std::string(service) + '/' + observed->id;
    const auto old = lookup(refs, archive);
    require(!old || *old == *observed, "native observation archive reference is occupied");
    updates.push_back({archive, old, observed});
  }
  require(store.compare_update_refs(updates, options), "native remote observation changed; retry synchronization");
  observed = root;
  return true;
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
  WorkspaceCapture capture;
  std::optional<ObjectKey> observed_head;
  std::optional<std::string> endpoint;
  bool rescan = true;
  std::optional<bool> clean;
  Impl(const Store &store, const fs::path &path, NativePushSyncOptions options)
      : storage(fs::canonical(store.root())), directory(fs::canonical(path)), workspace(store.native_workspace_name()),
        target(store.native_target()), settings(std::move(options)), observation(directory, storage), capture(directory, storage) {
    validate_ref(settings.remote_branch);
    require(settings.remote_branch.find('/') == settings.remote_branch.npos && settings.remote_branch.size() <= 64,
        "native remote names must be single components of at most 64 bytes");
    store.validate_native_workspace(directory);
    require(!settings.merge_changes || settings.receive_changes, "native agent merge requires reception");
    require(!settings.conflict_copies || (settings.receive_changes && settings.merge_changes),
        "native agent conflict copies require bidirectional merges");
    require(!settings.renames || (settings.receive_changes && settings.merge_changes),
        "native agent rename detection requires bidirectional merges");
    if (settings.renames) settings.renames->validate();
  }
};
NativePushSync::NativePushSync(const Store &store, const fs::path &directory, NativePushSyncOptions settings)
    : impl_(std::make_unique<Impl>(store, directory, std::move(settings))) {}
NativePushSync::~NativePushSync() = default;
NativePushSync::NativePushSync(NativePushSync &&) noexcept = default;
NativePushSync &NativePushSync::operator=(NativePushSync &&) noexcept = default;
void NativePushSync::invalidate() { impl_->rescan = true; impl_->observation.invalidate(); impl_->capture.invalidate(); }
Json NativePushSync::tick(Store &store, const Remote &remote, const WorkspaceChanges &hints,
                         OperationOptions options, TransferOptions transfer) {
  PlainSyncRemote transport(remote);
  return tick(store, transport, hints, options, transfer);
}
Json NativePushSync::tick(Store &store, NativeSyncRemote &remote, const WorkspaceChanges &hints,
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
  const auto observed_ref = store.native_workspace_reference("sync/push/" + service + "/observed");
  const auto head_ref = native_revision_reference(store, state.target);
  const auto refs = store.refs();
  const auto saved = read_checkpoint(store, refs, state_ref, base_ref, binding);
  auto last_observed = lookup(refs, observed_ref);
  require(!last_observed || last_observed->kind == Kind::revision, "invalid native remote observation");
  if (last_observed) (void)store.metadata(Kind::revision, last_observed->id);
  auto head = lookup(refs, head_ref);
  require(!head || head->kind == Kind::revision, "branch must point to a revision");
  Json result{{"version", 1}, {"state", "idle"}, {"reason", Json{}}, {"service", service},
      {"local_revision", identifier(head)}, {"remote_revision", Json{}}, {"baseline", identifier(saved.baseline)},
      {"working_clean", state.clean ? Json(*state.clean) : Json{}}, {"observed", false}, {"full_scan", false},
      {"committed", false}, {"transfer", Json{}}, {"last_observed_remote", identifier(last_observed ? last_observed : saved.baseline)}};
  const auto blocked = [&](std::string_view reason) {
    result["state"] = "blocked"; result["reason"] = reason; return result;
  };
  const auto observe = [&](const ObjectKey &root, bool historical, OperationOptions control) {
    const bool accepted = observe_revision(store, observed_ref, last_observed, saved.baseline, root, service, historical, control);
    result["last_observed_remote"] = identifier(last_observed ? last_observed : saved.baseline);
    return accepted;
  };
  const PublicationObserver published = [&](const ObjectKey &root) {
    // Like the wire publication/completion boundary, durable fact recording is
    // not cancellable. Failure leaves the publishing journal resumable.
    require(observe(root, true, {}), "published native history conflicts with a retained observation");
  };
  const auto receive = [&](const ObjectKey &incoming) {
    auto plan = store.plan_checkout(state.directory, state.target, incoming.id, true, options);
    if (!plan["ready"].get<bool>()) {
      result["checkout"] = std::move(plan);
      return blocked("working_changes");
    }
    result["checkout"] = store.checkout_directory_owned(state.directory, state.target, incoming.id, service, options);
    acknowledge(store, saved, state_ref, base_ref, binding, incoming, head_ref, service, options);
    state.observation.invalidate(); state.rescan = true; state.clean.reset(); state.observed_head = incoming;
    result["state"] = "pulled"; result["local_revision"] = incoming.id; result["baseline"] = incoming.id;
    result["working_clean"] = Json{};
    return result;
  };
  const auto pending = store.checkout_status();
  if (pending["pending"].get<bool>()) {
    if (!state.settings.receive_changes || pending.value("owner", "") != service) return blocked("checkout_pending");
    if (!last_observed) return blocked("checkout_observation_missing");
    if (!store.native_ancestry(last_observed->id, pending["target"].get<std::string>(), {}, options)["source_is_ancestor"].get<bool>())
      return blocked("checkout_observation_mismatch");
    result["checkout"] = state.settings.conflict_copies
        ? store.recover_checkout_owned_preserving_files(state.directory, state.target, service, options)
        : store.recover_checkout_owned(state.directory, state.target, service, options);
    state.observation.invalidate(); state.rescan = true; state.clean.reset();
    state.observed_head = lookup(store.refs(), head_ref);
    result["state"] = "recovered"; result["local_revision"] = identifier(state.observed_head);
    result["working_clean"] = Json{};
    return result;
  }
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
      const auto captured = state.capture.capture(store, state.target, "Syrnck automatic workspace snapshot", ingest,
                                                &state.observation);
      head = ObjectKey{Kind::revision, captured.at("revision").get<std::string>()};
      result["capture"] = captured.at("capture");
      result["committed"] = true; result["local_revision"] = head->id;
      state.clean = true; result["working_clean"] = true;
      state.observed_head = head; state.rescan = false;
    }
  }
  if (head || state.settings.receive_changes) store.require_features({"resident-sync-v1"}, options);
  auto push_limits = transfer; push_limits.require_read_lease = false;
  // A new local root must not bypass an unknown publication of a previous root.
  // Upgrade a service's first durable frontier from surviving one-shot/older
  // completed receipts. Subsequent ticks need no repeated ancestry walks over
  // completed history: the publication observer advances the frontier first.
  const bool import_completed = !last_observed && !saved.baseline;
  const auto sessions = remote.sessions(store, state.settings.remote_branch, import_completed, options, transfer);
  require(sessions.size() <= 65536, "native push transfer session limit exceeded");
  for (const auto &session : sessions) {
    throw_if_cancelled(options.stop);
    if (session.state == "complete") {
      if (import_completed && session.root.kind == Kind::revision && store.contains(session.root)) published(session.root);
    } else if (session.state == "publishing" || session.state == "uncertain") {
      if (session.root.kind != Kind::revision) return blocked("transfer_conflict");
      (void)remote.resume(store, session.id, options, push_limits, published);
    }
  }
  const auto incoming = remote.observe(store, state.settings.remote_branch, options, transfer);
  if (incoming.blocked_reason) return blocked(*incoming.blocked_reason);
  auto observed = incoming.root;
  require(!observed || observed->kind == Kind::revision, "native push remote reference is not a revision");
  result["remote_revision"] = identifier(observed);
  if (!observed && (saved.baseline || last_observed)) return blocked("remote_deleted");
  if (head && observed == head) {
    if (!observe(*observed, false, options)) return blocked("remote_history_rewritten");
    acknowledge(store, saved, state_ref, base_ref, binding, *head, head_ref, service, options);
    result["baseline"] = head->id;
    result["state"] = state.clean && !*state.clean ? "uncommitted" : "synced";
    return result;
  }
  if (!head && !observed) { result["state"] = "empty"; return result; }
  if (!head && !state.settings.receive_changes) return blocked("remote_ahead");
  if (saved.baseline && (!head || !store.native_ancestry(saved.baseline->id, head->id, {}, options)["source_is_ancestor"].get<bool>()))
    return blocked("local_history_rewritten");
  if (observed) {
    const auto tracking_name = "agent-" + service.substr(0, 32);
    const auto tracking = lookup(store.refs(), "remotes/" + tracking_name + '/' + state.settings.remote_branch);
    if (tracking != observed && observed != saved.baseline) {
      auto pull_limits = transfer; pull_limits.require_upload_lease = false; pull_limits.asset_locks.clear();
      const auto fetched = remote.fetch(store, state.settings.remote_branch, tracking_name, options, pull_limits);
      observed = ObjectKey{Kind::revision, fetched["revision"].get<std::string>()};
      result["remote_revision"] = observed->id;
      result["transfer"] = fetched["transfer"];
    }
    if (!observe(*observed, false, options)) return blocked("remote_history_rewritten");
    if (observed == head) {
      acknowledge(store, saved, state_ref, base_ref, binding, *head, head_ref, service, options);
      result["state"] = state.clean && !*state.clean ? "uncommitted" : "synced";
      result["baseline"] = head->id;
      return result;
    }
    if (!head) return receive(*observed);
    const auto graph = store.native_ancestry(observed->id, head->id, {}, options);
    if (state.settings.receive_changes && saved.baseline &&
        !store.native_ancestry(saved.baseline->id, observed->id, {}, options)["source_is_ancestor"].get<bool>())
      return blocked("remote_history_rewritten");
    if (!graph["source_is_ancestor"].get<bool>()) {
      if (graph["target_is_ancestor"].get<bool>())
        return state.settings.receive_changes ? receive(*observed) : blocked("remote_ahead");
      if (!state.settings.merge_changes) return blocked("diverged");
      NativeMergeOptions merge_options; merge_options.text_merge = TextDiffOptions{};
      merge_options.checkout_owner = service;
      merge_options.conflict_copies = state.settings.conflict_copies;
      merge_options.renames = state.settings.renames;
      auto plan = store.plan_merge(state.directory, state.target, observed->id, merge_options, true, options);
      if (!plan["ready"].get<bool>()) {
        const auto reason = !plan["conflicts"].empty() ? "merge_conflict"
            : !plan["blockers"].empty() ? "history_conflict" : "working_changes";
        result["merge"] = std::move(plan);
        return blocked(reason);
      }
      result["merge"] = store.merge_directory(state.directory, state.target, head->id, observed->id,
          "Syrnck automatic remote merge", merge_options, options);
      head = ObjectKey{Kind::revision, store.native_head()["revision"].get<std::string>()};
      state.observation.invalidate(); state.rescan = true; state.clean.reset(); state.observed_head = head;
      result["merged"] = true; result["local_revision"] = head->id; result["working_clean"] = Json{};
    }
    if (saved.baseline && observed != saved.baseline &&
        store.native_ancestry(observed->id, saved.baseline->id, {}, options)["source_is_ancestor"].get<bool>())
      return blocked("remote_history_rewritten");
  }
  if (remote.conflicted(store, *head, state.settings.remote_branch))
    return blocked("transfer_conflict");
  result["transfer"] = remote.push_expected(store, *head, state.settings.remote_branch, observed, options, push_limits, published);
  acknowledge(store, saved, state_ref, base_ref, binding, *head, head_ref, service, options);
  result["state"] = "pushed";
  result["baseline"] = head->id;
  result["expected_remote_revision"] = identifier(observed);
  result["remote_revision"] = head->id;
  return result;
}
} // namespace syrnck::net
