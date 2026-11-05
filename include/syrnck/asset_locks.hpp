#pragma once
#include <syrnck/core.hpp>
namespace syrnck {
struct AssetLockClaim {
  std::string path;
  std::uint64_t fence = 0;
};
// Repository-local lease table. The caller supplies a trusted owner identity;
// this component does not authenticate users or enforce remote publications.
// Keep the Store's exclusive lock; callbacks must not reenter it.
class AssetLocks {
  const Store &store_;
  std::function<std::uint64_t()> clock_;

public:
  explicit AssetLocks(const Store &, std::function<std::uint64_t()> trusted_clock = {});
  Json list(const std::optional<std::string> &after = {}, std::size_t limit = 100,
            OperationOptions = {}) const;
  Json acquire_plan(std::string_view owner, const std::vector<std::string> &paths,
                    std::uint64_t seconds = 3600, OperationOptions = {}) const;
  Json acquire(std::string_view owner, const std::vector<std::string> &paths, std::uint64_t seconds = 3600,
               OperationOptions = {}) const;
  Json renew(std::string_view owner, const std::vector<AssetLockClaim> &, std::uint64_t seconds = 3600,
             OperationOptions = {}) const;
  Json release(std::string_view owner, const std::vector<AssetLockClaim> &, OperationOptions = {}) const;
  Json prune(bool remove = false, OperationOptions = {}) const;
};
} // namespace syrnck
