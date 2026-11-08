#pragma once
#include <syrnck/core.hpp>
#include <syrnck/i18n.hpp>
#include <syrnck/asset_locks.hpp>
namespace syrnck::net {
// The server may have committed its CAS, but the client received no usable reply.
struct PublicationUncertain : Error {
  using Error::Error;
};
// Synchronous durable bookkeeping after proven publication, before completing
// its session. The fact can be historical; it does not imply a current ref.
// Implementations must be idempotent. A failure preserves the unfinished journal.
using PublicationObserver = std::function<void(const ObjectKey &)>;
enum class ObjectCompression { automatic, disabled, zstd };
struct TransferOptions {
  std::size_t max_requests = 1;
  // Charges object uploads, compression staging and reserved response buffers,
  // not total RSS (codec contexts and transport internals have separate costs).
  std::size_t max_in_flight_bytes = 64 * 1024 * 1024;
  // Additional attempts for idempotent requests only; publication is never retried.
  std::size_t max_retries = 3;
  std::uint64_t retry_delay_ms = 250;
  // Object-body average targets shared by the operation. Zero means unlimited.
  std::uint64_t upload_bytes_per_second = 0, download_bytes_per_second = 0;
  bool require_upload_lease = false;
  bool require_read_lease = false;
  // zstd requires server support, but incompressible bodies still travel raw.
  ObjectCompression compression = ObjectCompression::automatic;
  // Push-only: refuse replacing an existing reference with another object.
  bool immutable_reference = false;
  std::vector<AssetLockClaim> asset_locks;
  void validate() const;
};
struct ServeOptions {
  std::uint64_t upload_lease_seconds = 3600;
  bool require_upload_lease = false;
  std::uint64_t read_lease_seconds = 3600;
  bool object_compression = true;
  // Strict identity/branch/write-path configuration. No legacy token fallback.
  std::optional<Json> access;
  void validate() const;
};
void serve(Store &, std::string_view token, std::string_view listen = "127.0.0.1:7437",
           Language language = Language::english, ServeOptions = {});
class Remote {
public:
  Remote(std::string url, std::string token, std::optional<fs::path> ca = {});
  std::string endpoint_fingerprint() const;
  Refs refs(OperationOptions = {}, TransferOptions = {}) const;
  Json identity(OperationOptions = {}) const;
  Json locks(std::string_view action, const Json &request = Json::object(), OperationOptions = {}) const;
  Json push(const Store &, const ObjectKey &, std::string_view remote_ref, OperationOptions = {},
            TransferOptions = {}) const;
  // Bind a new publication to an earlier observation. Null requires absence;
  // a different current value fails before any session/object writes. Resume
  // existing uncertain sessions explicitly, without recapturing an expectation.
  Json push_expected(const Store &, const ObjectKey &, std::string_view remote_ref,
                     const std::optional<ObjectKey> &expected, OperationOptions = {}, TransferOptions = {}) const;
  Json push_expected(const Store &, const ObjectKey &, std::string_view remote_ref,
                     const std::optional<ObjectKey> &expected, OperationOptions, TransferOptions,
                     const PublicationObserver &) const;
  Json pull(const Store &, const ObjectKey &, std::string_view local_ref, OperationOptions = {},
            TransferOptions = {}) const;
  Json resume(const Store &, std::string_view session, OperationOptions = {}, TransferOptions = {}) const;
  Json resume(const Store &, std::string_view session, OperationOptions, TransferOptions,
              const PublicationObserver &) const;

private:
  Json push_impl(const Store &, const ObjectKey &, std::string_view,
                 const std::optional<std::optional<ObjectKey>> &, OperationOptions, TransferOptions,
                 const PublicationObserver & = {}) const;
  Json resume_impl(const Store &, std::string_view, OperationOptions, TransferOptions,
                   const PublicationObserver &) const;
  std::string base_, token_;
  std::optional<fs::path> ca_;
  bool negotiate_compression(std::optional<bool> &, OperationOptions, const TransferOptions &) const;
  Bytes request(std::string_view method, std::string_view path, std::span<const std::uint8_t> body = {},
                OperationOptions = {}, const TransferOptions *retry = nullptr) const;
};
} // namespace syrnck::net
