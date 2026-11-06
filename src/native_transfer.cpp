#include "internal.hpp"
#include <syrnck/native_net.hpp>
namespace syrnck::net {
namespace {
void component(std::string_view value) {
  validate_ref(value);
  require(value.find('/') == value.npos && value.size() <= 64,
          "native remote names must be single components of at most 64 bytes");
}
std::optional<ObjectKey> lookup(const Refs &refs, const std::string &name) {
  const auto found = refs.find(name);
  return found == refs.end() ? std::optional<ObjectKey>{} : std::optional{found->second};
}
} // namespace
Json fetch_native_branch(const Store &store, const Remote &remote, std::string_view branch,
                         std::string_view name, OperationOptions options, TransferOptions limits) {
  throw_if_cancelled(options.stop);
  component(branch);
  component(name);
  require_no_native_checkout(store);
  const auto tracking = "remotes/" + std::string(name) + '/' + std::string(branch);
  const auto local = store.refs();
  const auto expected = lookup(local, tracking);
  require(!expected || expected->kind == Kind::revision, "invalid native remote tracking reference");
  const auto refs = remote.refs(options, limits);
  const auto found = refs.find(std::string(branch));
  require(found != refs.end() && found->second.kind == Kind::revision, "unknown native remote branch");
  const auto root = found->second;
  // The immutable download root is also the crash-safe handoff between the
  // resumable object transfer and the local tracking-reference transaction.
  const auto retained = "native-fetch/" + root.id;
  const auto existing = lookup(local, retained);
  require(!existing || *existing == root, "native fetch retention reference is occupied");
  std::vector<RefUpdate> updates{{tracking, expected, root}, {retained, root, root}};
  if (expected) {
    const auto archive = "archive/fetch/" + expected->id;
    const auto previous = lookup(local, archive);
    require(!previous || *previous == *expected, "native fetch archive reference is occupied");
    updates.push_back({archive, previous, expected});
  }
  auto transfer = remote.pull(store, root, retained, options, limits);
  const auto revision = store.metadata(Kind::revision, root.id);
  require(store.compare_update_refs(updates, options), "native remote tracking changed; retry fetch");
  return Json{{"version", 1},
              {"state", "fetched"},
              {"remote", name},
              {"branch", branch},
              {"reference", tracking},
              {"revision", root.id},
              {"previous", expected ? Json(expected->id) : Json{}},
              {"files", revision["files"].size()},
              {"transfer", std::move(transfer)}};
}
Json push_native_branch(const Store &store, const Remote &remote, std::string_view branch,
                        std::string_view destination, OperationOptions options, TransferOptions limits) {
  throw_if_cancelled(options.stop);
  component(branch);
  component(destination);
  require_no_native_checkout(store);
  const auto refs = store.refs();
  const auto root = lookup(refs, "heads/" + std::string(branch));
  require(root && root->kind == Kind::revision, "unknown native branch");
  const auto identity = remote.identity(options);
  require(identity.is_object() && identity.contains("id") && identity["id"].is_string(),
          "native push requires server identities and branch policies");
  auto transfer = remote.push(store, *root, destination, options, limits);
  return Json{{"version", 1},         {"state", "pushed"},
              {"branch", branch},     {"remote_branch", destination},
              {"revision", root->id}, {"transfer", std::move(transfer)}};
}
} // namespace syrnck::net
