#pragma once
#include <syrnck/core.hpp>
#include <syrnck/i18n.hpp>
namespace syrnck::net {
// The server may have committed its CAS, but the client received no usable reply.
struct PublicationUncertain : Error {
  using Error::Error;
};
struct TransferOptions {
  std::size_t max_requests = 1;
  // Charges object uploads plus reserved response buffers, not total RSS.
  std::size_t max_in_flight_bytes = 64 * 1024 * 1024;
  // Additional attempts for idempotent requests only; publication is never retried.
  std::size_t max_retries = 3;
  std::uint64_t retry_delay_ms = 250;
  // Object-body average targets shared by the operation. Zero means unlimited.
  std::uint64_t upload_bytes_per_second = 0, download_bytes_per_second = 0;
  void validate() const;
};
void serve(Store &, std::string_view token, std::string_view listen = "127.0.0.1:7437",
           Language language = Language::english);
class Remote {
public:
  Remote(std::string url, std::string token, std::optional<fs::path> ca = {});
  Refs refs(OperationOptions = {}, TransferOptions = {}) const;
  Json push(const Store &, const ObjectKey &, std::string_view remote_ref, OperationOptions = {},
            TransferOptions = {}) const;
  Json pull(const Store &, const ObjectKey &, std::string_view local_ref, OperationOptions = {},
            TransferOptions = {}) const;

private:
  std::string base_, token_;
  std::optional<fs::path> ca_;
  Bytes request(std::string_view method, std::string_view path, std::span<const std::uint8_t> body = {},
                OperationOptions = {}, const TransferOptions *retry = nullptr) const;
};
} // namespace syrnck::net
