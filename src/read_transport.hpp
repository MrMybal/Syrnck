#pragma once
#include "lease_renewal.hpp"
namespace syrnck::net::detail {
class ReadProtection {
public:
  ReadProtection(const Store &, TransferSession &, OperationOptions, TransferOptions, LeaseRequest);
  void acquire();
  void tick();
  void release();

private:
  const Store &store_;
  TransferSession &session_;
  OperationOptions operation_;
  TransferOptions limits_;
  LeaseRequest request_;
  bool acquired_ = false;
  std::optional<ReadLease> lease_;
  std::uint64_t seconds_ = 3600;
  std::chrono::steady_clock::time_point renew_{};
  // Last member: cancellation and join precede destruction of request state.
  std::unique_ptr<LeaseRenewal> pending_;
  std::string path() const;
  std::string body() const;
  void decode(const Bytes &, const std::optional<std::string> &fence = {});
};
} // namespace syrnck::net::detail
