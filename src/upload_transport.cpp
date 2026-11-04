#include "upload_transport.hpp"
namespace syrnck::net::detail {
UploadProtection::UploadProtection(const Store &store, TransferSession &session, OperationOptions operation,
                                   TransferOptions limits, LeaseRequest request)
    : store_(store), session_(session), operation_(std::move(operation)), limits_(limits),
      request_(std::move(request)) {}
UploadProtection::~UploadProtection() = default;
bool UploadProtection::supported() {
  if (!supported_) {
    try {
      auto caps = parse_json(request_("GET", "v1/capabilities", {}, operation_));
      require(caps.is_object() && caps.contains("upload_leases") &&
                  caps["upload_leases"].is_number_unsigned() &&
                  caps["upload_leases"].get<std::uint64_t>() <= 1,
              "invalid server capabilities");
      supported_ = caps["upload_leases"] == 1;
      if (caps.contains("upload_inventories")) {
        require(caps["upload_inventories"].is_number_unsigned() &&
                    caps["upload_inventories"].get<std::uint64_t>() <= 1 &&
                    (caps["upload_inventories"] == 0 || *supported_),
                "invalid server capabilities");
        inventories_ = caps["upload_inventories"] == 1;
      }
    } catch (const HttpStatusError &error) {
      if (error.status != 404)
        throw;
      supported_ = false;
    }
  }
  require(*supported_ || !limits_.require_upload_lease, "server does not support upload leases");
  // A saved lease must never silently downgrade to an unprotected legacy upload.
  require(*supported_ || !session_.upload_lease, "server no longer supports the saved upload lease");
  return *supported_;
}
std::string UploadProtection::path() const {
  return "v1/uploads/" + *session_.upload_lease;
}
void UploadProtection::decode(const Bytes &data) {
  auto value = parse_json(data);
  require(value.is_object() && value.size() == 2 && value.contains("lease") &&
              value.contains("lease_seconds") && value["lease_seconds"].is_number_unsigned(),
          "invalid upload lease response");
  auto lease = UploadLease::parse(value["lease"]);
  require(session_.upload_lease && lease.id == *session_.upload_lease && lease.root == session_.root &&
              lease.reference == session_.reference && lease.expected == session_.expected,
          "upload lease descriptor mismatch");
  seconds_ = value["lease_seconds"].get<std::uint64_t>();
  require(seconds_ >= 1 && seconds_ <= 86400, "invalid upload lease response");
  lease_ = std::move(lease);
  renew_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(seconds_ * 1000 / 3);
}
bool UploadProtection::receipt() {
  if (!session_.upload_lease)
    return false;
  if (!supported())
    return false;
  try {
    decode(request_("GET", path(), {}, operation_));
  } catch (const HttpStatusError &error) {
    if (error.status != 404)
      throw;
    lease_.reset();
  }
  return lease_ && lease_->state == "published";
}
void UploadProtection::acquire() {
  if (!supported())
    return;
  if (!session_.upload_lease) {
    session_.upload_lease = hex(random_bytes(32));
    store_.write_transfer_session(session_, operation_.stop); // Persist identity before any remote creation.
  } else {
    (void)receipt();
  }
  require(!lease_ || !lease_->inventory_pages || inventories_,
          "server no longer supports the saved upload inventory");
  auto payload = body();
  received(request_("PUT", path(), bytes(payload), operation_));
}
std::string UploadProtection::body() const {
  auto old = lease_ ? std::optional(lease_->fence) : std::nullopt;
  auto value = Json{{"root", key_json(session_.root)},
                    {"reference", session_.reference},
                    {"expected", session_.expected ? key_json(*session_.expected) : Json(nullptr)},
                    {"fence", old ? Json(*old) : Json(nullptr)}};
  if (lease_ ? lease_->inventory_pages.has_value() : inventories_)
    value["inventory"] = true;
  return value.dump();
}
void UploadProtection::announce(std::uint32_t page, const Json &objects) {
  if (!lease_ || !lease_->inventory_pages)
    return;
  auto payload = Json{{"fence", lease_->fence}, {"objects", objects}}.dump();
  (void)request_("PUT", path() + "/inventory/" + std::to_string(page), bytes(payload), operation_);
}
void UploadProtection::received(const Bytes &data, const std::optional<std::string> &expected_fence) {
  decode(data);
  if (expected_fence && *expected_fence != lease_->fence)
    throw Error("upload lease expired or changed; resume to renegotiate objects");
  require(lease_->state == "active" || lease_->state == "published", "upload lease was released");
}
void UploadProtection::tick() {
  if (pending_) {
    if (pending_->future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
      return;
    auto done = std::move(pending_);
    received(done->future.get(), done->fence);
  }
  if (lease_ && lease_->state == "active" && std::chrono::steady_clock::now() >= renew_)
    pending_ = std::make_unique<LeaseRenewal>(request_, path(), body(), lease_->fence, operation_.stop);
}
void UploadProtection::finish() {
  tick();
  while (pending_) {
    throw_if_cancelled(operation_.stop);
    wait_until(std::chrono::steady_clock::now() + std::chrono::milliseconds(50), operation_.stop,
               [&] { tick(); });
  }
}
void UploadProtection::confirm() {
  if (!lease_ || lease_->state == "published")
    return;
  auto body = Json{{"fence", lease_->fence}}.dump();
  decode(request_("POST", path() + "/confirm", bytes(body), operation_));
  require(lease_->state == "published", "invalid upload lease response");
}
Json UploadProtection::publication() const {
  if (!lease_)
    return nullptr;
  return Json{{"id", lease_->id}, {"fence", lease_->fence}};
}
} // namespace syrnck::net::detail
