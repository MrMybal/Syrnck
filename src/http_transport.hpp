#pragma once
#include "internal.hpp"
#include <syrnck/net.hpp>
#include <curl/curl.h>
#include <exception>

namespace syrnck::net::detail {
struct HttpStatusError : Error {
  long status;
  explicit HttpStatusError(long status)
      : Error("HTTP request failed with status " + std::to_string(status)), status(status) {}
};
class HttpRequest {
public:
  HttpRequest(std::string_view base, std::string_view token, const std::optional<fs::path> &ca,
              std::string method, std::string path, Bytes body, OperationOptions,
              std::size_t response_limit = max_object_size, std::exception_ptr *first_error = nullptr);
  ~HttpRequest();
  HttpRequest(const HttpRequest &) = delete;
  HttpRequest &operator=(const HttpRequest &) = delete;
  CURL *handle() const;
  void attach(CURLM *);
  void detach();
  Bytes perform();
  Bytes finish(CURLcode);
  std::size_t charge() const;
  std::size_t uploaded_bytes() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
struct TransferStats {
  std::size_t peak_requests = 0, peak_buffered_bytes = 0;
};
using TransferNext = std::function<std::optional<ObjectKey>()>;
using TransferPrepare = std::function<Bytes(const ObjectKey &)>;
using TransferComplete = std::function<void(const ObjectKey &, Bytes, std::size_t)>;
// Every callback and Store operation stays on the calling coordinator thread.
void transfer_objects(std::string_view base, std::string_view token, const std::optional<fs::path> &ca,
                      bool upload, const TransferOptions &, const OperationOptions &, TransferStats &,
                      const TransferNext &, const TransferPrepare &, const TransferComplete &);
} // namespace syrnck::net::detail
