#include "internal.hpp"
#include <curl/curl.h>
#include <exception>
#include <httplib.h>
#include <iostream>
#include <monocypher.h>
#include <mutex>
#include <syrnck/net.hpp>
namespace syrnck::net {
namespace {
struct HttpTransferError : Error {
  using Error::Error;
};
struct HttpStatusError : Error {
  long status;
  explicit HttpStatusError(long status)
      : Error("HTTP request failed with status " + std::to_string(status)), status(status) {}
};
struct TransferContext {
  OperationOptions options;
  std::string_view path;
  Bytes response;
  std::exception_ptr error{};
  std::uint64_t bytes = 0;
  std::optional<std::uint64_t> total{};
  void notify(bool complete = false) const {
    report_progress(options, {ProgressPhase::transferring, path, bytes, total, complete});
  }
};
struct CurlGlobal {
  CurlGlobal() {
    require(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK, "curl initialization failed");
  }
  ~CurlGlobal() {
    curl_global_cleanup();
  }
};
void curl_init() {
  static CurlGlobal init;
  (void)init;
}
using Url = std::unique_ptr<CURLU, decltype(&curl_url_cleanup)>;
std::optional<std::string> url_part(CURLU *url, CURLUPart part) {
  char *result = nullptr;
  auto code = curl_url_get(url, part, &result, 0);
  if (code != CURLUE_OK)
    return {};
  std::unique_ptr<char, decltype(&curl_free)> value(result, &curl_free);
  return std::string(result);
}
bool loopback(std::string_view host) {
  if (host == "localhost" || host == "::1" || host == "[::1]")
    return true;
  std::string s(host);
  if (s.ends_with('.'))
    return false;
  std::array<unsigned, 4> parts{};
  std::size_t p = 0;
  for (auto &part : parts) {
    auto n = s.find('.', p);
    auto sub = s.substr(p, n == s.npos ? s.size() - p : n - p);
    if (sub.empty() || sub.size() > 3 ||
        !std::all_of(sub.begin(), sub.end(), [](char c) { return c >= '0' && c <= '9'; }))
      return false;
    part = static_cast<unsigned>(std::stoul(sub));
    if (part > 255)
      return false;
    if (&part != &parts.back() && n == s.npos)
      return false;
    p = n == s.npos ? s.size() : n + 1;
  }
  return p == s.size() && parts[0] == 127;
}
void validate_token(std::string_view token) {
  require(token.size() >= 32, "token must contain at least 32 characters");
  for (unsigned char c : token)
    require(c >= 33 && c <= 126, "token contains invalid characters");
}
Json refs_json(const Refs &refs) {
  Json j = Json::object();
  for (const auto &[name, key] : refs)
    j[name] = key_json(key);
  return j;
}
Refs decode_refs(const Json &j) {
  require(j.is_object(), "invalid references response");
  Refs refs;
  for (const auto &[name, key] : j.items()) {
    validate_ref(name);
    refs.emplace(name, parse_key(key));
  }
  return refs;
}
} // namespace
Remote::Remote(std::string url, std::string token, std::optional<fs::path> ca)
    : token_(std::move(token)), ca_(std::move(ca)) {
  curl_init();
  validate_token(token_);
  Url parsed(curl_url(), &curl_url_cleanup);
  require(parsed && curl_url_set(parsed.get(), CURLUPART_URL, url.c_str(), 0) == CURLUE_OK,
          "invalid server URL");
  auto scheme = url_part(parsed.get(), CURLUPART_SCHEME), host = url_part(parsed.get(), CURLUPART_HOST);
  require(scheme && host, "server URL requires a host");
  require(!url_part(parsed.get(), CURLUPART_USER) && !url_part(parsed.get(), CURLUPART_PASSWORD) &&
              !url_part(parsed.get(), CURLUPART_QUERY) && !url_part(parsed.get(), CURLUPART_FRAGMENT),
          "credentials, query and fragment are not allowed in server URL");
  require(*scheme == "https" || (*scheme == "http" && loopback(*host)),
          "HTTPS is required except on loopback");
  base_ = *url_part(parsed.get(), CURLUPART_URL);
  if (!base_.ends_with('/'))
    base_ += '/';
}
Bytes Remote::request(std::string_view method, std::string_view path, std::span<const std::uint8_t> body,
                      OperationOptions options) const {
  throw_if_cancelled(options.stop);
  using Handle = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
  Handle curl(curl_easy_init(), &curl_easy_cleanup);
  require(curl != nullptr, "cannot create HTTP client");
  std::string url = base_ + std::string(path), verb(method), auth = "Authorization: Bearer " + token_;
  curl_slist *list = nullptr;
  list = curl_slist_append(list, auth.c_str());
  list = curl_slist_append(list, "Content-Type: application/json");
  std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(list, &curl_slist_free_all);
  TransferContext context{std::move(options), path, {}};
  auto set = [&](CURLoption option, auto value) {
    require(curl_easy_setopt(curl.get(), option, value) == CURLE_OK, "HTTP client option failed");
  };
  set(CURLOPT_URL, url.c_str());
  set(CURLOPT_CUSTOMREQUEST, verb.c_str());
  set(CURLOPT_HTTPHEADER, list);
  set(CURLOPT_FOLLOWLOCATION, 0L);
  set(CURLOPT_PROTOCOLS_STR, "http,https");
  set(CURLOPT_CONNECTTIMEOUT, 15L);
  set(CURLOPT_TIMEOUT, 120L);
  set(CURLOPT_NOSIGNAL, 1L);
  set(CURLOPT_SSL_VERIFYPEER, 1L);
  set(CURLOPT_SSL_VERIFYHOST, 2L);
  // Never forward the shared token to an environment-configured HTTP proxy.
  set(CURLOPT_PROXY, "");
  std::string ca;
  if (ca_) {
    ca = utf8(*ca_);
    set(CURLOPT_CAINFO, ca.c_str());
  }
  if (method != "GET") {
    set(CURLOPT_POSTFIELDS, body.empty() ? "" : reinterpret_cast<const char *>(body.data()));
    set(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
  }
  using Callback = std::size_t (*)(char *, std::size_t, std::size_t, void *);
  Callback callback = +[](char *data, std::size_t size, std::size_t n, void *opaque) -> std::size_t {
    auto &state = *static_cast<TransferContext *>(opaque);
    auto &out = state.response;
    if (size != 0 && n > max_object_size / size)
      return 0;
    auto count = size * n;
    if (count > max_object_size - out.size())
      return 0;
    try {
      throw_if_cancelled(state.options.stop);
      out.insert(out.end(), reinterpret_cast<std::uint8_t *>(data),
                 reinterpret_cast<std::uint8_t *>(data) + count);
    } catch (...) {
      if (!state.error)
        state.error = std::current_exception();
      return 0;
    }
    return count;
  };
  set(CURLOPT_WRITEFUNCTION, callback);
  set(CURLOPT_WRITEDATA, &context);
  if (context.options.stop.stop_possible() || context.options.progress) {
    using Progress = int (*)(void *, curl_off_t, curl_off_t, curl_off_t, curl_off_t);
    Progress progress = +[](void *opaque, curl_off_t down_total, curl_off_t down_now, curl_off_t up_total,
                            curl_off_t up_now) -> int {
      auto &state = *static_cast<TransferContext *>(opaque);
      try {
        auto positive = [](curl_off_t n) { return static_cast<std::uint64_t>(std::max<curl_off_t>(0, n)); };
        state.bytes = positive(down_now) + positive(up_now);
        auto total = positive(down_total) + positive(up_total);
        state.total = total ? std::optional(total) : std::nullopt;
        state.notify();
        return 0;
      } catch (...) {
        if (!state.error)
          state.error = std::current_exception();
        return 1;
      }
    };
    set(CURLOPT_NOPROGRESS, 0L);
    set(CURLOPT_XFERINFOFUNCTION, progress);
    set(CURLOPT_XFERINFODATA, &context);
  }
  context.notify();
  auto status = curl_easy_perform(curl.get());
  if (context.error)
    std::rethrow_exception(context.error);
  throw_if_cancelled(context.options.stop);
  if (status != CURLE_OK)
    throw HttpTransferError(std::string("HTTP transfer failed: ") + curl_easy_strerror(status));
  long code = 0;
  curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &code);
  if (code < 200 || code >= 300)
    throw HttpStatusError(code);
  context.bytes = body.size() + context.response.size();
  context.total = context.bytes;
  context.notify(true);
  return std::move(context.response);
}
Refs Remote::refs(OperationOptions options) const {
  return decode_refs(parse_json(request("GET", "v1/refs", {}, std::move(options))));
}
Json Remote::push(const Store &store, const ObjectKey &root, std::string_view name,
                  OperationOptions options) const {
  throw_if_cancelled(options.stop);
  validate_ref(name);
  require(name.find('/') == name.npos, "remote reference must be a single component");
  auto remote = refs(options);
  Json expected = remote.contains(std::string(name)) ? key_json(remote.at(std::string(name))) : Json(nullptr);
  auto closure = store.closure({root}, true, options.stop, options.progress);
  std::vector<ObjectKey> objects(closure.begin(), closure.end());
  std::size_t sent = 0, reused = 0;
  std::uint64_t total = 0;
  for (std::size_t start = 0; start < objects.size(); start += 4096) {
    throw_if_cancelled(options.stop);
    auto end = std::min(objects.size(), start + 4096);
    Json batch = Json::array();
    std::set<ObjectKey> allowed;
    for (auto i = start; i < end; ++i) {
      batch.push_back(key_json(objects[i]));
      allowed.insert(objects[i]);
    }
    auto body = batch.dump();
    auto missing = parse_json(request("POST", "v1/missing", bytes(body), options));
    require(missing.is_array(), "invalid missing response");
    std::set<ObjectKey> unique;
    for (const auto &value : missing) {
      auto key = parse_key(value);
      require(allowed.contains(key), "server requested an unannounced object");
      unique.insert(key);
    }
    reused += end - start - unique.size();
    for (const auto &key : unique) {
      throw_if_cancelled(options.stop);
      auto data = store.get(key);
      (void)request("PUT", "v1/objects/" + std::string(kind_name(key.kind)) + "/" + key.id, data, options);
      ++sent;
      total += data.size();
    }
  }
  auto body = Json{{"root", key_json(root)}, {"expected", expected}}.dump();
  report_progress(options, {ProgressPhase::publishing, name});
  // The final CAS is a commit point. Once dispatched, finish receiving its
  // result instead of aborting a request the server may already have applied.
  try {
    (void)request("PUT", "v1/refs/" + std::string(name), bytes(body));
  } catch (const HttpStatusError &error) {
    if (error.status != 401 && error.status != 403 && error.status != 404 && error.status != 409)
      throw PublicationUncertain(
          "remote reference publication outcome is unknown; inspect the remote reference before retrying");
    throw;
  } catch (...) {
    throw PublicationUncertain(
        "remote reference publication outcome is unknown; inspect the remote reference before retrying");
  }
  return Json{{"transferred_objects", sent}, {"transferred_bytes", total}, {"reused_objects", reused}};
}
Json Remote::pull(const Store &store, const ObjectKey &root, std::string_view name,
                  OperationOptions options) const {
  throw_if_cancelled(options.stop);
  validate_ref(name);
  std::vector<ObjectKey> pending{root};
  std::set<ObjectKey> seen;
  std::size_t sent = 0, reused = 0;
  std::uint64_t total = 0;
  while (!pending.empty()) {
    throw_if_cancelled(options.stop);
    auto key = pending.back();
    pending.pop_back();
    if (!seen.insert(key).second)
      continue;
    Bytes data;
    if (store.contains(key)) {
      ++reused;
      data = store.get(key);
    } else {
      data = request("GET", "v1/objects/" + std::string(kind_name(key.kind)) + "/" + key.id, {}, options);
      throw_if_cancelled(options.stop);
      store.import(key, data);
      ++sent;
      total += data.size();
    }
    auto children = dependencies(key.kind, data);
    pending.insert(pending.end(), children.begin(), children.end());
  }
  report_progress(options, {ProgressPhase::publishing, name});
  store.set_ref(name, root, options.stop);
  return Json{{"transferred_objects", sent}, {"transferred_bytes", total}, {"reused_objects", reused}};
}
void serve(Store &store, std::string_view token, std::string_view listen, Language language) {
  validate_token(token);
  auto colon = listen.rfind(':');
  require(colon != listen.npos, "listen address must include a port");
  auto host = std::string(listen.substr(0, colon));
  if (host.starts_with('[') && host.ends_with(']'))
    host = host.substr(1, host.size() - 2);
  require(loopback(host) && host != "localhost",
          "HTTP server must bind to a loopback IP; publish behind a TLS proxy");
  auto port_text = std::string(listen.substr(colon + 1));
  require(!port_text.empty() &&
              std::all_of(port_text.begin(), port_text.end(), [](char c) { return c >= '0' && c <= '9'; }),
          "invalid port");
  auto port = std::stoi(port_text);
  require(port >= 0 && port <= 65535, "invalid port");
  httplib::Server server;
  server.new_task_queue = []() { return new httplib::ThreadPool(16); };
  server.set_payload_max_length(max_object_size);
  server.set_read_timeout(30, 0);
  server.set_write_timeout(120, 0);
  std::mutex mutex;
  auto expected = unhex(hash(bytes(token)));
  server.set_pre_routing_handler([&](const httplib::Request &request, httplib::Response &response) {
    auto auth = request.get_header_value("Authorization");
    if (!auth.starts_with("Bearer ") ||
        crypto_verify32(unhex(hash(bytes(std::string_view(auth).substr(7)))).data(), expected.data()) != 0) {
      response.status = 401;
      return httplib::Server::HandlerResponse::Handled;
    }
    return httplib::Server::HandlerResponse::Unhandled;
  });
  auto guarded = [&](auto handler) {
    return [&, handler](const httplib::Request &request, httplib::Response &response) {
      std::lock_guard lock(mutex);
      try {
        handler(request, response);
      } catch (const MissingObject &e) {
        response.status = 404;
        response.set_content(e.what(), "text/plain");
      } catch (const std::exception &e) {
        response.status = 400;
        response.set_content(e.what(), "text/plain");
      }
    };
  };
  server.Post("/v1/missing", guarded([&](const auto &request, auto &response) {
                auto input = parse_json(bytes(request.body));
                require(input.is_array() && input.size() <= 4096, "batch exceeds 4096 objects or is invalid");
                Json missing = Json::array();
                for (const auto &value : input) {
                  auto key = parse_key(value);
                  if (!store.contains(key))
                    missing.push_back(key_json(key));
                }
                response.set_content(missing.dump(), "application/json");
              }));
  server.Get(R"(/v1/objects/(chunk|manifest|revision)/([a-f0-9]{64}))",
             guarded([&](const auto &request, auto &response) {
               ObjectKey key{parse_kind(request.matches[1].str()), request.matches[2].str()};
               if (!store.contains(key)) {
                 response.status = 404;
                 return;
               }
               auto data = store.get(key);
               response.set_content(text(data), "application/octet-stream");
             }));
  server.Put(R"(/v1/objects/(chunk|manifest|revision)/([a-f0-9]{64}))",
             guarded([&](const auto &request, auto &response) {
               ObjectKey key{parse_kind(request.matches[1].str()), request.matches[2].str()};
               response.status = store.import(key, bytes(request.body)) ? 201 : 204;
             }));
  server.Get("/v1/refs", guarded([&](const auto &, auto &response) {
               response.set_content(refs_json(store.refs()).dump(), "application/json");
             }));
  server.Put(R"(/v1/refs/([A-Za-z0-9._-]+))", guarded([&](const auto &request, auto &response) {
               auto name = request.matches[1].str();
               validate_ref(name);
               auto j = parse_json(bytes(request.body));
               require(j.is_object() && j.size() == 2, "invalid publication request");
               auto root = parse_key(j.at("root"));
               std::optional<ObjectKey> previous;
               if (!j.at("expected").is_null())
                 previous = parse_key(j.at("expected"));
               response.status = store.compare_set_ref(name, previous, root) ? 204 : 409;
             }));
  std::cerr << localize("Syrnck listening at http://", language) << host << ':' << port << '\n';
  require(server.listen(host, port), "HTTP server failed to listen");
}
} // namespace syrnck::net
