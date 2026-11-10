#include "sealed_internal.hpp"
#include "native_graph.hpp"
#include "sealed_publication.hpp"
#include <syrnck/sealed_net.hpp>
#include <algorithm>
namespace syrnck::net {
namespace {
std::optional<ObjectKey> lookup(const Refs &refs, std::string_view name) {
  const auto found = refs.find(std::string(name));
  return found == refs.end() ? std::optional<ObjectKey>{} : std::optional{found->second};
}
void validate(const Store &wire, const Store &plain, std::string_view channel, const SealedLimits &limits,
              const TransferOptions &transfer, OperationOptions options) {
  throw_if_cancelled(options.stop);
  limits.validate();
  transfer.validate();
  sealed_detail::channel_name(channel);
  sealed_detail::distinct_stores(plain, wire);
  require_no_native_checkout(plain);
  require(transfer.asset_locks.empty(), "sealed transfers cannot assert plaintext asset lock claims");
}
void push_identity(const Remote &remote, OperationOptions options) {
  const auto identity = remote.identity(options);
  require(identity.is_object() && identity.contains("id") && identity["id"].is_string(),
          "sealed push requires server identities and branch policies");
}
void fast_forward(const Store &plain, const Refs &old, const Refs &next, const SealedLimits &limits,
                  OperationOptions options) {
  std::vector<std::string> tips;
  std::vector<std::pair<std::string, std::string>> changes;
  for (const auto &[name, before] : old) {
    throw_if_cancelled(options.stop);
    const auto after = lookup(next, name);
    if (after == before)
      continue;
    require(after && before.kind == Kind::revision && after->kind == Kind::revision,
            "sealed root deletion or replacement requires --allow-rewrite");
    tips.push_back(before.id);
    tips.push_back(after->id);
    changes.emplace_back(before.id, after->id);
  }
  if (changes.empty())
    return;
  NativeAncestryGraph graph(
      plain, tips,
      {std::min<std::size_t>(limits.max_objects, 1000000), std::min<std::size_t>(limits.max_work, 10000000)},
      options);
  sealed_detail::Budget budget{limits, options};
  for (const auto &[before, after] : changes)
    require(graph.compare({before}, {after}, options, [&](std::size_t work) { budget.work(work); })
                .source_is_ancestor,
            "sealed original history diverged; merge or explicitly allow a rewrite");
}
} // namespace
PublicationObserver sealed_transfer_detail::observer(const Store &wire, const Store &plain,
                                                     const SecretKey &key, std::string channel,
                                                     std::string checkpoint, const SealedLimits &limits,
                                                     ProgressCallback progress) {
  return [&, channel = std::move(channel), checkpoint = std::move(checkpoint),
          progress](const ObjectKey &root) {
    require(root.kind == Kind::revision, "invalid sealed publication root");
    // Publication is already proven: finish durable bookkeeping even if the
    // caller cancelled meanwhile. Never reinterpret a newer checkpoint as stale.
    sealed_detail::Cipher cipher(key);
    const auto accepted = sealed_detail::read_checkpoint(plain, cipher, channel, checkpoint, plain.refs());
    if (accepted && accepted->id != root.id) {
      sealed_detail::Budget budget{limits, {{}, progress}};
      const auto current = sealed_detail::read_root(wire, cipher, accepted->id, channel, budget);
      if (sealed_detail::descends_from(wire, cipher, current, root.id, channel, budget))
        return;
    }
    (void)open_sealed_snapshot(wire, plain, key, root.id, channel, checkpoint, limits, {{}, progress});
  };
}
namespace {
std::string retained(const ObjectKey &root) { return "sealed/download/" + root.id; }
} // namespace
std::string sealed_remote_checkpoint(const Remote &remote, std::string_view channel) {
  sealed_detail::channel_name(channel);
  return "sealed/checkpoints/" + remote.endpoint_fingerprint() + '/' + std::string(channel);
}
Json fetch_sealed_snapshot(const Store &wire, const Store &destination, const Remote &remote,
                           const SecretKey &key, std::string_view channel, const SealedLimits &limits,
                           OperationOptions options, TransferOptions transfer) {
  validate(wire, destination, channel, limits, transfer, options);
  const auto found = lookup(remote.refs(options, transfer), channel);
  require(found && found->kind == Kind::revision, "unknown sealed remote channel");
  auto downloaded = remote.pull(wire, *found, retained(*found), options, transfer);
  auto opened = open_sealed_snapshot(wire, destination, key, found->id, channel,
                                     sealed_remote_checkpoint(remote, channel), limits, options);
  opened["state"] = "fetched";
  opened["transfer"] = std::move(downloaded);
  return opened;
}
Json push_sealed_snapshot(const Store &source, const Store &wire, const Remote &remote, const SecretKey &key,
                          std::string_view channel, const Refs &roots, bool allow_rewrite,
                          const SealedLimits &limits, OperationOptions options, TransferOptions transfer) {
  validate(wire, source, channel, limits, transfer, options);
  require(!transfer.require_read_lease, "read lease requirement is only available for pull");
  const auto endpoint = remote.endpoint_fingerprint();
  for (const auto &session : wire.transfer_sessions(options.stop))
    require(session.direction != "push" || session.endpoint != endpoint || session.reference != channel ||
                session.state == "complete",
            "unfinished sealed publication exists; resume its exact session");
  push_identity(remote, options);
  const auto checkpoint = sealed_remote_checkpoint(remote, channel);
  const auto expected = lookup(remote.refs(options, transfer), channel);
  sealed_detail::Cipher cipher(key);
  const auto accepted = sealed_detail::read_checkpoint(source, cipher, channel, checkpoint, source.refs());
  require(expected || !accepted, "sealed remote channel disappeared after an accepted checkpoint");
  std::optional<std::string> previous;
  if (expected) {
    require(expected->kind == Kind::revision, "invalid sealed remote root");
    auto download = transfer;
    download.require_upload_lease = false;
    download.immutable_reference = false;
    remote.pull(wire, *expected, retained(*expected), options, download);
    const auto opened =
        open_sealed_snapshot(wire, source, key, expected->id, channel, checkpoint, limits, options);
    if (!allow_rewrite)
      fast_forward(source, parse_refs(bytes(opened["roots"].dump()), "invalid sealed roots"), roots, limits,
                   options);
    previous = expected->id;
  }
  auto prepared = seal_snapshot(source, wire, key, channel, roots, previous, limits, options);
  const ObjectKey root{Kind::revision, prepared["snapshot"].get<std::string>()};
  auto published = sealed_transfer_detail::observer(wire, source, key, std::string(channel), checkpoint,
                                                    limits, options.progress);
  auto sent = remote.push_expected(wire, root, channel, expected, options, transfer, published);
  prepared["state"] = "pushed";
  prepared["checkpoint"] = checkpoint;
  prepared["transfer"] = std::move(sent);
  return prepared;
}
Json resume_sealed_transfer(const Store &wire, const Store &destination, const Remote &remote,
                            const SecretKey &key, std::string_view channel, std::string_view session_id,
                            const SealedLimits &limits, OperationOptions options, TransferOptions transfer) {
  validate(wire, destination, channel, limits, transfer, options);
  const auto session = wire.transfer_session(session_id);
  require(session && session->endpoint == remote.endpoint_fingerprint() &&
              session->root.kind == Kind::revision,
          "sealed transfer session does not match the endpoint");
  const auto checkpoint = sealed_remote_checkpoint(remote, channel);
  Json sent;
  if (session->direction == "push") {
    require(session->reference == channel, "sealed transfer session does not match the channel");
    push_identity(remote, options);
    // Authenticate even completed sessions before recording publication locally.
    (void)inspect_sealed_snapshot(wire, key, session->root.id, channel, limits, options);
    sent = remote.resume(wire, session_id, options, transfer,
                         sealed_transfer_detail::observer(wire, destination, key, std::string(channel),
                                                          checkpoint, limits, options.progress));
  } else {
    require(session->reference == retained(session->root), "invalid sealed download retention reference");
    sent = remote.resume(wire, session_id, options, transfer);
    auto opened =
        open_sealed_snapshot(wire, destination, key, session->root.id, channel, checkpoint, limits, options);
    opened["state"] = "resumed";
    opened["transfer"] = std::move(sent);
    return opened;
  }
  auto result = inspect_sealed_snapshot(wire, key, session->root.id, channel, limits, options);
  result["state"] = "resumed";
  result["checkpoint"] = checkpoint;
  result["transfer"] = std::move(sent);
  return result;
}
} // namespace syrnck::net
