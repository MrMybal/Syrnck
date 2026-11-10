#include "sealed_internal.hpp"
#include "sealed_publication.hpp"
#include <syrnck/sealed_net.hpp>
namespace syrnck::net {
namespace {
std::optional<ObjectKey> lookup(const Refs &refs, std::string_view name) {
  const auto at = refs.find(std::string(name));
  return at == refs.end() ? std::optional<ObjectKey>{} : std::optional{at->second};
}
class SealedSyncRemote final : public NativeSyncRemote {
public:
  SealedSyncRemote(const Store &wire, const Remote &remote, const SecretKey &key, std::string channel,
                   std::string private_root, SealedLimits limits)
      : wire_(wire), remote_(remote), key_(key), channel_(std::move(channel)),
        private_root_(std::move(private_root)), limits_(limits), cipher_(key) {
    limits_.validate();
    sealed_detail::channel_name(channel_);
    validate_ref(private_root_);
    fingerprint_ = hash(bytes(Json{{"version", 1},
                                   {"transport", "sealed-native-sync"},
                                   {"endpoint", remote_.endpoint_fingerprint()},
                                   {"realm", cipher_.realm},
                                   {"private_root", private_root_}}
                                  .dump()));
    checkpoint_ = sealed_remote_checkpoint(remote_, channel_);
  }
  std::string endpoint_fingerprint() const override { return fingerprint_; }
  std::vector<NativeSyncSession> sessions(const Store &plain, std::string_view branch, bool include_completed,
                                          OperationOptions options, TransferOptions transfer) override {
    validate(plain, branch, options, transfer);
    const auto all = wire_.transfer_sessions(options.stop);
    require(all.size() <= 65536, "native push transfer session limit exceeded");
    std::vector<NativeSyncSession> result;
    sessions_.clear();
    for (const auto &session : all) {
      if (session.direction != "push" || session.endpoint != remote_.endpoint_fingerprint() ||
          session.reference != channel_ || (session.state == "complete" && !include_completed))
        continue;
      require(session.root.kind == Kind::revision, "invalid sealed publication root");
      const auto root = read_root(session.root.id, options);
      const auto original = selected(root);
      // A wire journal pins only ciphertext. Keep its original revision alive
      // across source GC, rewrites and restoration before using a logical fact.
      retain_candidate(plain, root, options);
      if (session.state == "complete") {
        ensure_accepted_wire(plain, options, transfer);
        sealed_transfer_detail::observer(wire_, plain, key_, channel_, checkpoint_, limits_,
                                         options.progress)(session.root);
      }
      result.push_back({session.id, original, session.state});
      sessions_.push_back(session);
    }
    return result;
  }
  NativeSyncObservation observe(const Store &plain, std::string_view branch, OperationOptions options,
                                TransferOptions transfer) override {
    validate(plain, branch, options, transfer);
    observed_ = false;
    const auto accepted = sealed_detail::read_checkpoint(plain, cipher_, channel_, checkpoint_, plain.refs());
    if (accepted)
      (void)selected(*accepted);
    expected_wire_ = lookup(remote_.refs(options, transfer), channel_);
    expected_original_.reset();
    if (!expected_wire_) {
      if (accepted)
        return {{}, "remote_deleted"};
      observed_ = true;
      return {};
    }
    require(expected_wire_->kind == Kind::revision, "invalid sealed remote root");
    download(*expected_wire_, options, transfer);
    const auto root = read_root(expected_wire_->id, options);
    expected_original_ = selected(root);
    if (accepted) {
      sealed_detail::Budget budget{limits_, options};
      if (!sealed_detail::descends_from(wire_, cipher_, root, accepted->id, channel_, budget))
        return {expected_original_, "remote_history_rewritten"};
    }
    if (!accepted || accepted->id != root.id) {
      opened_ = open_sealed_snapshot(wire_, plain, key_, root.id, channel_, checkpoint_, limits_, options);
    } else {
      require(accepted->roots == root.roots, "sealed checkpoint roots changed");
      (void)plain.metadata(Kind::revision, expected_original_->id);
    }
    observed_ = true;
    return {expected_original_, {}};
  }
  Json fetch(const Store &plain, std::string_view branch, std::string_view name, OperationOptions options,
             TransferOptions transfer) override {
    validate(plain, branch, options, transfer);
    require(observed_ && expected_original_, "sealed synchronization requires a remote observation");
    validate_ref(name);
    require(name.find('/') == name.npos && name.size() <= 64,
            "native remote names must be single components of at most 64 bytes");
    const auto tracking = "remotes/" + std::string(name) + '/' + channel_;
    const auto refs = plain.refs();
    const auto before = lookup(refs, tracking);
    require(!before || before->kind == Kind::revision, "invalid native remote tracking reference");
    std::vector<RefUpdate> updates{{tracking, before, expected_original_}};
    if (before) {
      const auto archive = "archive/fetch/" + before->id;
      const auto old = lookup(refs, archive);
      require(!old || old == before, "native fetch archive reference is occupied");
      updates.push_back({archive, old, before});
    }
    require(plain.compare_update_refs(updates, options), "native remote tracking changed; retry fetch");
    return Json{{"revision", expected_original_->id}, {"transfer", downloaded_}, {"sealed", opened_}};
  }
  Json resume(const Store &plain, std::string_view session_id, OperationOptions options,
              TransferOptions transfer, const PublicationObserver &published) override {
    validate(plain, channel_, options, transfer);
    const auto session = wire_.transfer_session(session_id);
    require(session && session->direction == "push" && session->endpoint == remote_.endpoint_fingerprint() &&
                session->reference == channel_ && session->root.kind == Kind::revision,
            "sealed transfer session does not match the endpoint");
    push_identity(options);
    (void)inspect_sealed_snapshot(wire_, key_, session->root.id, channel_, limits_, options);
    retain_candidate(plain, read_root(session->root.id, options), options);
    ensure_accepted_wire(plain, options, transfer);
    return remote_.resume(wire_, session_id, options, transfer, observer(plain, published, options.progress));
  }
  bool conflicted(const Store &, const ObjectKey &original, std::string_view branch) const override {
    require(branch == channel_, "sealed synchronization channel changed");
    for (const auto &session : sessions_)
      if (session.state == "conflict" && selected(read_root(session.root.id, {})) == original)
        return true;
    return false;
  }
  Json push_expected(const Store &plain, const ObjectKey &original, std::string_view branch,
                     const std::optional<ObjectKey> &expected, OperationOptions options,
                     TransferOptions transfer, const PublicationObserver &published) override {
    validate(plain, branch, options, transfer);
    require(observed_ && expected == expected_original_,
            "sealed synchronization requires a remote observation");
    require(original.kind == Kind::revision, "branch must point to a revision");
    push_identity(options);
    std::optional<ObjectKey> prepared;
    for (const auto &listed : sessions_) {
      const auto current = wire_.transfer_session(listed.id);
      require(current.has_value(), "sealed transfer session disappeared");
      const auto &session = *current;
      if (session.state == "complete" || session.state == "conflict")
        continue;
      require(session.state != "publishing" && session.state != "uncertain",
              "unfinished sealed publication exists; resume its exact session");
      const auto candidate = read_root(session.root.id, options);
      if (selected(candidate) == original && candidate.previous == previous()) {
        require(session.expected == expected_wire_, "sealed transfer expectation changed");
        prepared = session.root;
        break;
      }
    }
    if (!prepared) {
      const auto report = seal_snapshot(plain, wire_, key_, channel_, {{private_root_, original}}, previous(),
                                        limits_, options);
      prepared = ObjectKey{Kind::revision, report["snapshot"].get<std::string>()};
    }
    retain_candidate(plain, read_root(prepared->id, options), options);
    // Never recapture the server's current value here: this is the exact wire
    // expectation authenticated by observe(), before any local merge/checkout.
    return remote_.push_expected(wire_, *prepared, channel_, expected_wire_, options, transfer,
                                 observer(plain, published, options.progress));
  }

private:
  const Store &wire_;
  const Remote &remote_;
  const SecretKey &key_;
  std::string channel_, private_root_, fingerprint_, checkpoint_;
  SealedLimits limits_;
  sealed_detail::Cipher cipher_;
  std::vector<TransferSession> sessions_;
  bool observed_ = false;
  std::optional<ObjectKey> expected_wire_, expected_original_;
  Json downloaded_, opened_;
  void validate(const Store &plain, std::string_view branch, OperationOptions options,
                const TransferOptions &transfer) const {
    throw_if_cancelled(options.stop);
    transfer.validate();
    require(branch == channel_, "sealed synchronization channel changed");
    sealed_detail::distinct_stores(plain, wire_);
    require_no_native_checkout(plain);
    require(transfer.asset_locks.empty(), "sealed transfers cannot assert plaintext asset lock claims");
  }
  sealed_detail::Root read_root(std::string_view id, OperationOptions options) const {
    sealed_detail::Budget budget{limits_, options};
    return sealed_detail::read_root(wire_, cipher_, id, channel_, budget);
  }
  ObjectKey selected(const sealed_detail::Root &root) const {
    const auto original = lookup(root.roots, private_root_);
    require(root.roots.size() == 1 && original && original->kind == Kind::revision,
            "sealed synchronization requires exactly the configured private revision root");
    return *original;
  }
  std::optional<std::string> previous() const {
    return expected_wire_ ? std::optional{expected_wire_->id} : std::nullopt;
  }
  void push_identity(OperationOptions options) const {
    const auto identity = remote_.identity(options);
    require(identity.is_object() && identity.contains("id") && identity["id"].is_string(),
            "sealed push requires server identities and branch policies");
  }
  void download(const ObjectKey &root, OperationOptions options, TransferOptions transfer) {
    // Accepted idle polls authenticate the small root control again, without
    // re-enumerating an unchanged closure or issuing any object-body requests.
    const auto refs = wire_.refs();
    const auto retained = "sealed/download/" + root.id;
    const auto cached = lookup(refs, retained);
    require(!cached || *cached == root, "sealed download retention reference is occupied");
    if (cached == root || lookup(refs, "sealed/prepared/" + root.id) == root)
      return;
    transfer.require_upload_lease = false;
    transfer.immutable_reference = false;
    downloaded_ = remote_.pull(wire_, root, "sealed/download/" + root.id, options, transfer);
  }
  void ensure_accepted_wire(const Store &plain, OperationOptions options, TransferOptions transfer) {
    const auto accepted = sealed_detail::read_checkpoint(plain, cipher_, channel_, checkpoint_, plain.refs());
    if (accepted)
      download({Kind::revision, accepted->id}, options, transfer);
  }
  void retain_candidate(const Store &plain, const sealed_detail::Root &root, OperationOptions options) const {
    const auto original = selected(root);
    const auto pin = "sealed/agent-candidates/" + fingerprint_ + '/' + root.id;
    const auto found = lookup(plain.refs(), pin);
    require(!found || *found == original, "sealed candidate retention reference is occupied");
    if (found) {
      (void)plain.metadata(Kind::revision, original.id);
      return;
    }
    if (!plain.contains(original))
      (void)open_sealed_snapshot(wire_, plain, key_, root.id, channel_,
                                 "sealed/checkpoints/agent-candidates/" + fingerprint_ + '/' + root.id,
                                 limits_, options);
    require(plain.compare_update_refs({{pin, {}, original}}, options),
            "sealed candidate retention changed; reload and retry");
  }
  PublicationObserver observer(const Store &plain, PublicationObserver published, ProgressCallback progress) {
    auto checkpoint =
        sealed_transfer_detail::observer(wire_, plain, key_, channel_, checkpoint_, limits_, progress);
    return [this, &plain, checkpoint = std::move(checkpoint), published = std::move(published),
            progress](const ObjectKey &root) {
      require(root.kind == Kind::revision, "invalid sealed publication root");
      const auto control = read_root(root.id, {{}, progress});
      retain_candidate(plain, control, {{}, progress});
      checkpoint(root);
      // Both durable facts precede completion of the physical wire journal.
      if (published)
        published(selected(control));
    };
  }
};
} // namespace
std::unique_ptr<NativeSyncRemote> sealed_native_sync(const Store &wire, const Remote &remote,
                                                     const SecretKey &key, std::string channel,
                                                     std::string private_root, const SealedLimits &limits) {
  return std::make_unique<SealedSyncRemote>(wire, remote, key, std::move(channel), std::move(private_root),
                                            limits);
}
} // namespace syrnck::net
