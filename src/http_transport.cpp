#include "http_transport.hpp"
#include <algorithm>
#include <map>

namespace syrnck::net::detail {
namespace {
constexpr std::size_t upload_response_limit = 4096;
void multi_check(CURLMcode code) {
  require(code == CURLM_OK, std::string("HTTP multi operation failed: ") + curl_multi_strerror(code));
}
} // namespace
struct HttpRequest::Impl {
  OperationOptions options;
  std::string path;
  Bytes body, response;
  std::size_t response_limit;
  std::exception_ptr error{};
  std::exception_ptr *first_error;
  std::uint64_t bytes = 0;
  std::optional<std::uint64_t> total{};
  std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers{nullptr, &curl_slist_free_all};
  std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl{nullptr, &curl_easy_cleanup};
  CURLM *multi = nullptr;
  void notify(bool complete = false) const {
    report_progress(options, {ProgressPhase::transferring, path, bytes, total, complete});
  }
  void remember_error() noexcept {
    if (!error)
      error = std::current_exception();
    if (first_error && !*first_error)
      *first_error = error;
  }
  Impl(std::string_view base, std::string_view token, const std::optional<fs::path> &ca, std::string method,
       std::string relative, Bytes payload, OperationOptions operation, std::size_t limit,
       std::exception_ptr *first)
      : options(std::move(operation)), path(std::move(relative)), body(std::move(payload)),
        response_limit(limit), first_error(first) {
    throw_if_cancelled(options.stop);
    curl.reset(curl_easy_init());
    require(curl != nullptr, "cannot create HTTP client");
    auto append = [&](const char *header) {
      auto *next = curl_slist_append(headers.get(), header);
      require(next != nullptr, "cannot allocate HTTP headers");
      (void)headers.release();
      headers.reset(next);
    };
    auto auth = "Authorization: Bearer " + std::string(token);
    append(auth.c_str());
    append("Content-Type: application/json");
    auto set = [&](CURLoption option, auto value) {
      require(curl_easy_setopt(curl.get(), option, value) == CURLE_OK, "HTTP client option failed");
    };
    auto url = std::string(base) + path;
    set(CURLOPT_URL, url.c_str());
    set(CURLOPT_CUSTOMREQUEST, method.c_str());
    set(CURLOPT_HTTPHEADER, headers.get());
    set(CURLOPT_FOLLOWLOCATION, 0L);
    set(CURLOPT_PROTOCOLS_STR, "http,https");
    set(CURLOPT_CONNECTTIMEOUT, 15L);
    set(CURLOPT_TIMEOUT, 120L);
    set(CURLOPT_NOSIGNAL, 1L);
    set(CURLOPT_SSL_VERIFYPEER, 1L);
    set(CURLOPT_SSL_VERIFYHOST, 2L);
    set(CURLOPT_PROXY, "");
    if (ca) {
      auto name = utf8(*ca);
      set(CURLOPT_CAINFO, name.c_str());
    }
    if (method != "GET") {
      set(CURLOPT_POSTFIELDS, body.empty() ? "" : reinterpret_cast<const char *>(body.data()));
      set(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
    }
    using Write = std::size_t (*)(char *, std::size_t, std::size_t, void *);
    Write write = +[](char *data, std::size_t size, std::size_t n, void *opaque) -> std::size_t {
      auto &state = *static_cast<Impl *>(opaque);
      if (size && n > state.response_limit / size)
        return 0;
      auto count = size * n;
      if (count > state.response_limit - state.response.size())
        return 0;
      try {
        throw_if_cancelled(state.options.stop);
        // Multi requests reserve their worst-case response credit once. This
        // avoids transient old+new vector buffers during geometric growth.
        if (state.first_error && count && state.response.capacity() == 0)
          state.response.reserve(state.response_limit);
        state.response.insert(state.response.end(), reinterpret_cast<std::uint8_t *>(data),
                              reinterpret_cast<std::uint8_t *>(data) + count);
      } catch (...) {
        state.remember_error();
        return 0;
      }
      return count;
    };
    set(CURLOPT_WRITEFUNCTION, write);
    set(CURLOPT_WRITEDATA, this);
    if (options.stop.stop_possible() || options.progress) {
      using Progress = int (*)(void *, curl_off_t, curl_off_t, curl_off_t, curl_off_t);
      Progress progress = +[](void *opaque, curl_off_t down_total, curl_off_t down_now, curl_off_t up_total,
                              curl_off_t up_now) -> int {
        auto &state = *static_cast<Impl *>(opaque);
        try {
          auto positive = [](curl_off_t n) { return static_cast<std::uint64_t>(std::max<curl_off_t>(0, n)); };
          state.bytes = positive(down_now) + positive(up_now);
          auto total = positive(down_total) + positive(up_total);
          state.total = total ? std::optional(total) : std::nullopt;
          state.notify();
          return 0;
        } catch (...) {
          state.remember_error();
          return 1;
        }
      };
      set(CURLOPT_NOPROGRESS, 0L);
      set(CURLOPT_XFERINFOFUNCTION, progress);
      set(CURLOPT_XFERINFODATA, this);
    }
    notify();
  }
  ~Impl() {
    if (multi)
      (void)curl_multi_remove_handle(multi, curl.get());
  }
};
HttpRequest::HttpRequest(std::string_view base, std::string_view token, const std::optional<fs::path> &ca,
                         std::string method, std::string path, Bytes body, OperationOptions options,
                         std::size_t limit, std::exception_ptr *first)
    : impl_(std::make_unique<Impl>(base, token, ca, std::move(method), std::move(path), std::move(body),
                                   std::move(options), limit, first)) {}
HttpRequest::~HttpRequest() = default;
CURL *HttpRequest::handle() const {
  return impl_->curl.get();
}
void HttpRequest::attach(CURLM *multi) {
  multi_check(curl_multi_add_handle(multi, handle()));
  impl_->multi = multi;
}
void HttpRequest::detach() {
  multi_check(curl_multi_remove_handle(impl_->multi, handle()));
  impl_->multi = nullptr;
}
Bytes HttpRequest::perform() {
  return finish(curl_easy_perform(handle()));
}
Bytes HttpRequest::finish(CURLcode code) {
  if (impl_->error)
    std::rethrow_exception(impl_->error);
  throw_if_cancelled(impl_->options.stop);
  if (code != CURLE_OK)
    throw Error(std::string("HTTP transfer failed: ") + curl_easy_strerror(code));
  long status = 0;
  require(curl_easy_getinfo(handle(), CURLINFO_RESPONSE_CODE, &status) == CURLE_OK,
          "HTTP response inspection failed");
  if (status < 200 || status >= 300)
    throw HttpStatusError(status);
  impl_->bytes = impl_->body.size() + impl_->response.size();
  impl_->total = impl_->bytes;
  impl_->notify(true);
  return std::move(impl_->response);
}
std::size_t HttpRequest::charge() const {
  return impl_->body.capacity() + impl_->response_limit;
}
std::size_t HttpRequest::uploaded_bytes() const {
  return impl_->body.size();
}
void transfer_objects(std::string_view base, std::string_view token, const std::optional<fs::path> &ca,
                      bool upload, const TransferOptions &limits, const OperationOptions &operation,
                      TransferStats &stats, const TransferNext &next, const TransferPrepare &prepare,
                      const TransferComplete &complete) {
  limits.validate();
  throw_if_cancelled(operation.stop);
  std::unique_ptr<CURLM, decltype(&curl_multi_cleanup)> multi(curl_multi_init(), &curl_multi_cleanup);
  require(multi != nullptr, "cannot create HTTP transfer scheduler");
  multi_check(
      curl_multi_setopt(multi.get(), CURLMOPT_MAX_TOTAL_CONNECTIONS, static_cast<long>(limits.max_requests)));
  multi_check(
      curl_multi_setopt(multi.get(), CURLMOPT_MAX_HOST_CONNECTIONS, static_cast<long>(limits.max_requests)));
  std::exception_ptr first_error;
  struct Active {
    ObjectKey key;
    std::unique_ptr<HttpRequest> request;
    std::size_t charge;
  };
  std::map<CURL *, Active> active;
  std::size_t charged = 0;
  const auto response_limit = upload ? upload_response_limit : max_object_size;
  const auto reservation = response_limit + (upload ? max_object_size : 0);
  for (;;) {
    throw_if_cancelled(operation.stop);
    while (active.size() < limits.max_requests && reservation <= limits.max_in_flight_bytes - charged) {
      auto key = next();
      if (!key)
        break;
      throw_if_cancelled(operation.stop);
      auto payload = upload ? prepare(*key) : Bytes{};
      require(payload.capacity() <= limits.max_in_flight_bytes - charged - response_limit,
              "transfer buffer budget exceeded");
      auto path = "v1/objects/" + std::string(kind_name(key->kind)) + "/" + key->id;
      auto request =
          std::make_unique<HttpRequest>(base, token, ca, upload ? "PUT" : "GET", std::move(path),
                                        std::move(payload), operation, response_limit, &first_error);
      auto charge = request->charge();
      auto *handle = request->handle();
      request->attach(multi.get());
      active.emplace(handle, Active{std::move(*key), std::move(request), charge});
      charged += charge;
      stats.peak_requests = std::max(stats.peak_requests, active.size());
      stats.peak_buffered_bytes = std::max(stats.peak_buffered_bytes, charged);
    }
    if (active.empty())
      return;
    int running = 0;
    multi_check(curl_multi_perform(multi.get(), &running));
    if (first_error)
      std::rethrow_exception(first_error);
    throw_if_cancelled(operation.stop);
    int remaining = 0;
    bool finished = false;
    while (auto *message = curl_multi_info_read(multi.get(), &remaining)) {
      if (message->msg != CURLMSG_DONE)
        continue;
      auto found = active.find(message->easy_handle);
      require(found != active.end(), "unknown HTTP transfer completion");
      const auto code = message->data.result; // Copy before detach invalidates the message.
      auto job = std::move(found->second);
      active.erase(found);
      job.request->detach();
      auto data = job.request->finish(code);
      complete(job.key, std::move(data), job.request->uploaded_bytes());
      charged -= job.charge;
      finished = true;
    }
    if (!finished && !active.empty())
      multi_check(curl_multi_poll(multi.get(), nullptr, 0, 100, nullptr));
  }
}
} // namespace syrnck::net::detail
