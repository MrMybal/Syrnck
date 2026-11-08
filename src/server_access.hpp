#pragma once
#include "internal.hpp"
#include <syrnck/asset_locks.hpp>
namespace syrnck::net::detail {
struct AccessDenied : Error {
  using Error::Error;
};
struct Principal {
  std::string id, digest, role;
  std::vector<std::string> paths, branches;
};
struct BranchPolicy {
  bool fast_forward = true;
  std::vector<std::string> locks;
  bool require_signatures = false;
};
class ServerAccess {
  struct Signer {
    std::size_t principal;
    std::string public_key, name, email;
  };
  std::vector<Principal> principals_;
  std::map<std::string, BranchPolicy> branches_;
  std::map<std::string, Signer> signers_;
  const Principal &revision_signer(const Json &, std::string_view branch) const;

public:
  explicit ServerAccess(const Json &);
  const Principal *authenticate(std::string_view token) const;
  bool visible(std::string_view name) const;
  void write_path(const Principal &, std::string_view) const;
  void write_branch(const Principal &, std::string_view) const;
  // Called while holding the server mutex and the Store's exclusive lock.
  // The returned callback checks lease time again after closure verification.
  std::function<void(const ProgressEvent &)> publication(const Store &, const Principal &,
                                                         std::string_view name, const ObjectKey &,
                                                         const std::optional<ObjectKey> &,
                                                         const std::vector<AssetLockClaim> &) const;
};
std::vector<AssetLockClaim> decode_lock_claims(const Json &);
} // namespace syrnck::net::detail
