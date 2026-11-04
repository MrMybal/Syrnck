#include "git_internal.hpp"
#include <syrnck/git_net.hpp>
namespace syrnck::git {
std::string catalog_reference(std::string_view commit) {
  detail::validate_oid(commit);
  return "git-" + std::string(commit);
}
namespace {
Json result(const Catalog &catalog, const ObjectKey &root, Json transfer) {
  return Json{{"commit", catalog.commit},
              {"assets", catalog.assets.size()},
              {"bundle", key_json(root)},
              {"transfer", std::move(transfer)}};
}
} // namespace
Json push_assets(const Store &store, const Catalog &catalog, const net::Remote &remote,
                 OperationOptions operation, net::TransferOptions limits) {
  limits.validate();
  require(!limits.require_read_lease, "read lease requirement is only available for pull");
  auto root = prepare_bundle(store, catalog, operation);
  limits.immutable_reference = true;
  return result(catalog, root,
                remote.push(store, root, catalog_reference(catalog.commit), operation, limits));
}
Json pull_assets(const Store &store, const Catalog &catalog, const net::Remote &remote,
                 OperationOptions operation, net::TransferOptions limits) {
  limits.validate();
  require(!limits.require_upload_lease, "upload lease requirement is only available for push");
  require(!limits.immutable_reference, "immutable reference option is only available for push");
  auto root = bundle_key(catalog);
  auto name = "git-assets/" + catalog.commit;
  auto local = store.refs();
  std::optional<ObjectKey> expected;
  if (auto found = local.find(name); found != local.end())
    expected = found->second;
  require(!expected || *expected == root, "local Git catalog points to another object");
  auto references = remote.refs(operation, limits);
  auto found = references.find(catalog_reference(catalog.commit));
  require(found != references.end() && found->second == root, "remote Git catalog does not match commit");
  auto stage = "git-stage/" + root.id;
  auto transferred = remote.pull(store, root, stage, operation, limits);
  // Size contracts belong to Git pointers. Check them before publishing the
  // final catalog pin; a failed check keeps the staged, verified closure.
  verify_assets(store, catalog, operation);
  report_progress(operation, {ProgressPhase::publishing, name});
  require(store.compare_set_ref(name, expected, root, operation.stop),
          "local Git catalog changed during transfer");
  store.remove_ref(stage); // Finish cleanup after the final publication boundary.
  return result(catalog, root, std::move(transferred));
}
} // namespace syrnck::git
