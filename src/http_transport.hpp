#pragma once
#include "internal.hpp"
#include <syrnck/net.hpp>
#include <curl/curl.h>
#include <exception>
#include <chrono>

namespace syrnck::net::detail {
struct HttpStatusError : Error {
  long status;
  std::chrono::milliseconds retry_after;
  explicit HttpStatusError(long status, std::chrono::milliseconds delay = {})
      : Error("HTTP request failed with status " + std::to_string(status)), status(status),
        retry_after(delay) {}
};
struct HttpTransportError : Error {
  CURLcode code;
  explicit HttpTransportError(CURLcode code)
      : Error(std::string("HTTP transfer failed: ") + curl_easy_strerror(code)), code(code) {}
};
std::optional<std::chrono::milliseconds> retry_delay(const std::exception_ptr &, std::size_t retries,
                                                     const TransferOptions &);
void wait_until(std::chrono::steady_clock::time_point, std::stop_token,
                const std::function<void()> &tick = {});
struct TrafficBudget {
  explicit TrafficBudget(const TransferOptions &);
  void account(std::uint64_t uploaded, std::uint64_t downloaded);
  std::chrono::steady_clock::time_point ready() const;
  std::uint64_t uploaded_bytes() const {
    return uploaded_;
  }
  std::uint64_t downloaded_bytes() const {
    return downloaded_;
  }

private:
  std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
  std::uint64_t upload_rate_, download_rate_, uploaded_ = 0, downloaded_ = 0;
};
class HttpRequest {
public:
  HttpRequest(std::string_view base, std::string_view token, const std::optional<fs::path> &ca,
              std::string method, std::string path, Bytes body, OperationOptions,
              std::size_t response_limit = max_object_size, std::exception_ptr *first_error = nullptr,
              TrafficBudget *traffic = nullptr, const TransferOptions *limits = nullptr,
              bool accept_zstd = false, bool encoded_body = false);
  ~HttpRequest();
  HttpRequest(const HttpRequest &) = delete;
  HttpRequest &operator=(const HttpRequest &) = delete;
  CURL *handle() const;
  void attach(CURLM *);
  void detach();
  Bytes perform();
  Bytes finish(CURLcode);
  std::size_t charge() const;
  void restart();
  void rate_share(const TransferOptions &, std::size_t slots);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
struct TransferStats {
  std::size_t peak_requests = 0, peak_buffered_bytes = 0;
  std::size_t retries = 0;
};
using TransferNext = std::function<std::optional<ObjectKey>()>;
using TransferPrepare = std::function<Bytes(const ObjectKey &)>;
using TransferComplete = std::function<void(const ObjectKey &, Bytes, std::size_t)>;
// Every callback and Store operation stays on the calling coordinator thread.
void transfer_objects(std::string_view base, std::string_view token, const std::optional<fs::path> &ca,
                      bool upload, const TransferOptions &, const OperationOptions &, TransferStats &,
                      TrafficBudget &, const TransferNext &, const TransferPrepare &,
                      const TransferComplete &, const std::function<void()> &tick = {},
                      const std::function<bool()> &compression = {});
} // namespace syrnck::net::detail
