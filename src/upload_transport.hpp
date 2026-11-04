#pragma once
#include "lease_renewal.hpp"
namespace syrnck::net::detail {
class UploadProtection {
public:
  UploadProtection(const Store &, TransferSession &, OperationOptions, TransferOptions, LeaseRequest);
  ~UploadProtection();
  bool receipt();
  void acquire();
  void announce(std::uint32_t page, const Json &objects);
  void tick();
  void finish();
  void confirm();
  bool published() const {
    return lease_ && lease_->state == "published";
  }
  Json publication() const;

private:
  const Store &store_;
  TransferSession &session_;
  OperationOptions operation_;
  TransferOptions limits_;
  LeaseRequest request_;
  std::optional<bool> supported_;
  bool inventories_ = false;
  std::optional<UploadLease> lease_;
  std::uint64_t seconds_ = 3600;
  std::chrono::steady_clock::time_point renew_{};
  std::unique_ptr<LeaseRenewal> pending_;
  bool supported();
  std::string path() const;
  void decode(const Bytes &);
  std::string body() const;
  void received(const Bytes &, const std::optional<std::string> &expected_fence = {});
};
} // namespace syrnck::net::detail
