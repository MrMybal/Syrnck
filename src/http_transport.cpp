#include "http_transport.hpp"
#include <algorithm>
#include <map>
#include <charconv>
#include <condition_variable>
#include <ctime>
#include <limits>
#include <mutex>

namespace syrnck::net::detail {
namespace {
constexpr std::size_t upload_response_limit = 4096;
void multi_check(CURLMcode code) {
  require(code == CURLM_OK, std::string("HTTP multi operation failed: ") + curl_multi_strerror(code));
}
} // namespace
std::optional<std::chrono::milliseconds> retry_delay(const std::exception_ptr &error, std::size_t retries,
                                                     const TransferOptions &limits) {
  if (retries >= limits.max_retries)
    return {};
  std::chrono::milliseconds server_delay{};
  try {
    std::rethrow_exception(error);
  } catch (const HttpStatusError &http) {
    if (http.status != 408 && http.status != 429 && http.status != 500 && http.status != 502 &&
        http.status != 503 && http.status != 504)
      return {};
    server_delay = http.retry_after;
  } catch (const HttpTransportError &transport) {
    switch (transport.code) {
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_CONNECT:
    case CURLE_OPERATION_TIMEDOUT:
    case CURLE_SEND_ERROR:
    case CURLE_RECV_ERROR:
    case CURLE_GOT_NOTHING:
    case CURLE_PARTIAL_FILE:
      break;
    default:
      return {};
    }
  } catch (...) {
    return {};
  }
  auto delay = std::chrono::milliseconds(std::min<std::uint64_t>(30000, limits.retry_delay_ms << retries));
  return std::max(delay, server_delay);
}
void wait_until(std::chrono::steady_clock::time_point deadline, std::stop_token stop) {
  throw_if_cancelled(stop);
  if (deadline <= std::chrono::steady_clock::now())
    return;
  std::mutex mutex;
  std::condition_variable_any changed;
  std::unique_lock lock(mutex);
  while (std::chrono::steady_clock::now() < deadline) {
    throw_if_cancelled(stop);
    changed.wait_until(lock, stop,
                       std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(100)),
                       [] { return false; });
  }
  throw_if_cancelled(stop);
}
TrafficBudget::TrafficBudget(const TransferOptions &limits)
    : upload_rate_(limits.upload_bytes_per_second), download_rate_(limits.download_bytes_per_second) {}
void TrafficBudget::account(std::uint64_t up, std::uint64_t down) {
  require(up <= std::numeric_limits<std::uint64_t>::max() - uploaded_ &&
              down <= std::numeric_limits<std::uint64_t>::max() - downloaded_,
          "transfer traffic counter overflow");
  uploaded_ += up;
  downloaded_ += down;
}
std::chrono::steady_clock::time_point TrafficBudget::ready() const {
  auto seconds = std::max(upload_rate_ ? static_cast<long double>(uploaded_) / upload_rate_ : 0,
                          download_rate_ ? static_cast<long double>(downloaded_) / download_rate_ : 0);
  auto duration = std::chrono::duration<long double>(seconds);
  auto maximum = std::chrono::steady_clock::time_point::max() - start_;
  return duration >= maximum
             ? std::chrono::steady_clock::time_point::max()
             : start_ + std::chrono::duration_cast<std::chrono::steady_clock::duration>(duration);
}
struct HttpRequest::Impl {
  OperationOptions options;
  std::string path;
  Bytes body, response;
  std::size_t response_limit;
  std::exception_ptr error{};
  std::exception_ptr *first_error;
  TrafficBudget *traffic;
  std::uint64_t uploaded = 0, downloaded = 0;
  std::chrono::milliseconds retry_after{};
  std::uint64_t bytes = 0;
  std::optional<std::uint64_t> total{};
  std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers{nullptr, &curl_slist_free_all};
  std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl{nullptr, &curl_easy_cleanup};
  CURLM *multi = nullptr;
  void account(curl_off_t up, curl_off_t down) {
    auto u = static_cast<std::uint64_t>(std::max<curl_off_t>(0, up));
    auto d = static_cast<std::uint64_t>(std::max<curl_off_t>(0, down));
    if (traffic)
      traffic->account(u > uploaded ? u - uploaded : 0, d > downloaded ? d - downloaded : 0);
    uploaded = std::max(uploaded, u);
    downloaded = std::max(downloaded, d);
  }
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
       std::exception_ptr *first, TrafficBudget *budget, const TransferOptions *limits)
      : options(std::move(operation)), path(std::move(relative)), body(std::move(payload)),
        response_limit(limit), first_error(first), traffic(budget) {
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
    // Include the worst-case throttled duration; the normal 120-second
    // allowance must not make deliberately slow transfers impossible.
    std::uint64_t timeout = 120;
    if (limits)
      for (auto rate : {limits->upload_bytes_per_second, limits->download_bytes_per_second})
        if (rate)
          timeout += (max_object_size * limits->max_requests + rate - 1) / rate;
    set(CURLOPT_TIMEOUT, static_cast<long>(timeout));
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
    // Retry-After is advisory and bounded. Ignore malformed/oversized fields.
    Write header = +[](char *data, std::size_t size, std::size_t n, void *opaque) -> std::size_t {
      auto &state = *static_cast<Impl *>(opaque);
      if (size && n > std::numeric_limits<std::size_t>::max() / size)
        return 0;
      const auto count = size * n;
      try {
        std::string_view line(data, count);
        if (line.starts_with("HTTP/"))
          state.retry_after = {};
        constexpr std::string_view name = "retry-after:";
        if (line.size() <= 128 && line.size() >= name.size() &&
            std::equal(name.begin(), name.end(), line.begin(),
                       [](char a, char b) { return a == (b >= 'A' && b <= 'Z' ? b + ('a' - 'A') : b); })) {
          line.remove_prefix(name.size());
          while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
            line.remove_prefix(1);
          while (!line.empty() &&
                 (line.back() == ' ' || line.back() == '\t' || line.back() == '\r' || line.back() == '\n'))
            line.remove_suffix(1);
          std::uint64_t seconds = 0;
          auto parsed = std::from_chars(line.data(), line.data() + line.size(), seconds);
          if (parsed.ec != std::errc{} || parsed.ptr != line.data() + line.size()) {
            auto date = curl_getdate(std::string(line).c_str(), nullptr);
            auto now = std::time(nullptr);
            seconds = date > now ? static_cast<std::uint64_t>(date - now) : 0;
          }
          state.retry_after = std::chrono::milliseconds(std::min<std::uint64_t>(seconds, 30) * 1000);
        }
      } catch (...) {
        state.remember_error();
        return 0;
      }
      return count;
    };
    set(CURLOPT_HEADERFUNCTION, header);
    set(CURLOPT_HEADERDATA, this);
    if (options.stop.stop_possible() || options.progress || traffic) {
      using Progress = int (*)(void *, curl_off_t, curl_off_t, curl_off_t, curl_off_t);
      Progress progress = +[](void *opaque, curl_off_t down_total, curl_off_t down_now, curl_off_t up_total,
                              curl_off_t up_now) -> int {
        auto &state = *static_cast<Impl *>(opaque);
        try {
          state.account(up_now, down_now);
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
                         std::size_t limit, std::exception_ptr *first, TrafficBudget *traffic,
                         const TransferOptions *limits)
    : impl_(std::make_unique<Impl>(base, token, ca, std::move(method), std::move(path), std::move(body),
                                   std::move(options), limit, first, traffic, limits)) {}
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
  curl_off_t uploaded = 0, downloaded = 0;
  require(curl_easy_getinfo(handle(), CURLINFO_SIZE_UPLOAD_T, &uploaded) == CURLE_OK &&
              curl_easy_getinfo(handle(), CURLINFO_SIZE_DOWNLOAD_T, &downloaded) == CURLE_OK,
          "HTTP response inspection failed");
  impl_->account(uploaded, downloaded);
  if (code != CURLE_OK)
    throw HttpTransportError(code);
  long status = 0;
  require(curl_easy_getinfo(handle(), CURLINFO_RESPONSE_CODE, &status) == CURLE_OK,
          "HTTP response inspection failed");
  if (status < 200 || status >= 300)
    throw HttpStatusError(status, impl_->retry_after);
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
void HttpRequest::restart() {
  impl_->response.clear(); // Retain the already charged capacity.
  impl_->error = {};
  impl_->bytes = impl_->uploaded = impl_->downloaded = 0;
  impl_->total = {};
  impl_->retry_after = {};
  impl_->notify();
}
void HttpRequest::rate_share(const TransferOptions &limits, std::size_t slots) {
  if (!limits.upload_bytes_per_second && !limits.download_bytes_per_second)
    return;
  require(curl_easy_setopt(handle(), CURLOPT_MAX_SEND_SPEED_LARGE,
                           static_cast<curl_off_t>(limits.upload_bytes_per_second / slots)) == CURLE_OK &&
              curl_easy_setopt(handle(), CURLOPT_MAX_RECV_SPEED_LARGE,
                               static_cast<curl_off_t>(limits.download_bytes_per_second / slots)) == CURLE_OK,
          "HTTP client option failed");
}
void transfer_objects(std::string_view base, std::string_view token, const std::optional<fs::path> &ca,
                      bool upload, const TransferOptions &limits, const OperationOptions &operation,
                      TransferStats &stats, TrafficBudget &traffic, const TransferNext &next,
                      const TransferPrepare &prepare, const TransferComplete &complete) {
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
    std::size_t retries = 0;
    std::chrono::steady_clock::time_point ready{};
  };
  std::map<CURL *, Active> active;
  std::vector<Active> waiting;
  std::size_t charged = 0;
  const auto response_limit = upload ? upload_response_limit : max_object_size;
  const auto reservation = response_limit + (upload ? max_object_size : 0);
  for (;;) {
    throw_if_cancelled(operation.stop);
    wait_until(traffic.ready(), operation.stop);
    for (auto it = waiting.begin(); it != waiting.end();) {
      if (it->ready > std::chrono::steady_clock::now()) {
        ++it;
        continue;
      }
      it->request->restart();
      auto *handle = it->request->handle();
      it->request->attach(multi.get());
      active.emplace(handle, std::move(*it));
      it = waiting.erase(it);
    }
    while (active.size() + waiting.size() < limits.max_requests &&
           reservation <= limits.max_in_flight_bytes - charged) {
      auto key = next();
      if (!key)
        break;
      throw_if_cancelled(operation.stop);
      auto payload = upload ? prepare(*key) : Bytes{};
      require(payload.capacity() <= limits.max_in_flight_bytes - charged - response_limit,
              "transfer buffer budget exceeded");
      auto path = "v1/objects/" + std::string(kind_name(key->kind)) + "/" + key->id;
      auto request = std::make_unique<HttpRequest>(base, token, ca, upload ? "PUT" : "GET", std::move(path),
                                                   std::move(payload), operation, response_limit,
                                                   &first_error, &traffic, &limits);
      auto charge = request->charge();
      auto *handle = request->handle();
      request->attach(multi.get());
      active.emplace(handle, Active{std::move(*key), std::move(request), charge});
      charged += charge;
      stats.peak_requests = std::max(stats.peak_requests, active.size() + waiting.size());
      stats.peak_buffered_bytes = std::max(stats.peak_buffered_bytes, charged);
    }
    if (active.empty()) {
      if (waiting.empty())
        return;
      auto first = std::min_element(waiting.begin(), waiting.end(),
                                    [](const auto &a, const auto &b) { return a.ready < b.ready; });
      wait_until(first->ready, operation.stop);
      continue;
    }
    for (auto &[handle, job] : active)
      job.request->rate_share(limits, active.size());
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
      Bytes data;
      try {
        data = job.request->finish(code);
      } catch (...) {
        auto delay = retry_delay(std::current_exception(), job.retries, limits);
        if (!delay)
          throw;
        ++job.retries;
        ++stats.retries;
        job.ready = std::chrono::steady_clock::now() + *delay;
        waiting.push_back(std::move(job));
        finished = true;
        continue;
      }
      complete(job.key, std::move(data), job.request->uploaded_bytes());
      charged -= job.charge;
      finished = true;
    }
    if (!finished && !active.empty())
      multi_check(curl_multi_poll(multi.get(), nullptr, 0, 100, nullptr));
  }
}
} // namespace syrnck::net::detail
