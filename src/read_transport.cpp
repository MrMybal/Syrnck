#include "read_transport.hpp"
namespace syrnck::net::detail {
ReadProtection::ReadProtection(const Store &store, TransferSession &session, OperationOptions operation,
                               TransferOptions limits, LeaseRequest request)
    : store_(store), session_(session), operation_(std::move(operation)), limits_(limits),
      request_(std::move(request)) {}
std::string ReadProtection::path() const {
  return "v1/reads/" + *session_.read_lease;
}
std::string ReadProtection::body() const {
  return Json{{"root", key_json(session_.root)}, {"fence", lease_ ? Json(lease_->fence) : Json(nullptr)}}
      .dump();
}
void ReadProtection::decode(const Bytes &data, const std::optional<std::string> &fence) {
  auto value = parse_json(data);
  require(value.is_object() && value.size() == 2 && value.contains("lease") &&
              value.contains("lease_seconds") && value["lease_seconds"].is_number_unsigned(),
          "invalid read lease response");
  auto lease = ReadLease::parse(value["lease"]);
  require(session_.read_lease && lease.id == *session_.read_lease && lease.root == session_.root,
          "read lease descriptor mismatch");
  seconds_ = value["lease_seconds"].get<std::uint64_t>();
  require(seconds_ >= 1 && seconds_ <= 86400, "invalid read lease response");
  if (fence && lease.fence != *fence)
    throw Error("read lease expired or changed; resume to reacquire protection");
  lease_ = std::move(lease);
  renew_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(seconds_ * 1000 / 3);
}
void ReadProtection::acquire() {
  if (acquired_)
    return;
  bool supported = false;
  try {
    auto caps = parse_json(request_("GET", "v1/capabilities", {}, operation_));
    require(caps.is_object(), "invalid server capabilities");
    require(caps.contains("read_leases") ||
                (caps.contains("upload_leases") && caps["upload_leases"].is_number_unsigned() &&
                 caps["upload_leases"].get<std::uint64_t>() <= 1),
            "invalid server capabilities");
    // Older upload-capable servers do not advertise the optional read extension.
    if (caps.contains("read_leases")) {
      require(caps["read_leases"].is_number_unsigned() && caps["read_leases"].get<std::uint64_t>() <= 1,
              "invalid server capabilities");
      supported = caps["read_leases"] == 1;
    }
  } catch (const HttpStatusError &error) {
    if (error.status != 404)
      throw;
  }
  require(supported || !limits_.require_read_lease, "server does not support read leases");
  require(supported || !session_.read_lease, "server no longer supports the saved read lease");
  if (supported) {
    if (!session_.read_lease) {
      session_.read_lease = hex(random_bytes(32));
      store_.write_transfer_session(session_, operation_.stop);
    } else {
      try {
        decode(request_("GET", path(), {}, operation_));
      } catch (const HttpStatusError &error) {
        if (error.status != 404)
          throw;
      }
    }
    auto payload = body();
    decode(request_("PUT", path(), bytes(payload), operation_));
    require(lease_->state == "active", "read lease was released");
  }
  acquired_ = true;
}
void ReadProtection::tick() {
  if (pending_) {
    if (pending_->future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
      return;
    auto done = std::move(pending_);
    decode(done->future.get(), done->fence);
    require(lease_->state == "active", "read lease was released");
  }
  if (lease_ && lease_->state == "active" && std::chrono::steady_clock::now() >= renew_)
    pending_ = std::make_unique<LeaseRenewal>(request_, path(), body(), lease_->fence, operation_.stop);
}
void ReadProtection::release() {
  // No source data is needed after the object scheduler drains. Cancel renewal before release.
  pending_.reset();
  if (!session_.read_lease)
    return;
  try {
    if (!lease_)
      decode(request_("GET", path(), {}, operation_));
    if (lease_->state == "released")
      return;
    auto payload = Json{{"fence", lease_->fence}}.dump();
    decode(request_("POST", path() + "/release", bytes(payload), operation_));
    require(lease_->state == "released", "invalid read lease response");
  } catch (const HttpStatusError &error) {
    if (error.status != 404)
      throw;
    // An already pruned record has no hold left to release.
  }
}
} // namespace syrnck::net::detail
