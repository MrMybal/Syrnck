#include "internal.hpp"
#include "http_transport.hpp"
#include <curl/curl.h>
#include <exception>
#include <httplib.h>
#include <iostream>
#include <monocypher.h>
#include <mutex>
#include <syrnck/net.hpp>
namespace syrnck::net {
namespace {
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
                      OperationOptions options, const TransferOptions *retry) const {
  Bytes payload;
  if (!body.empty())
    payload.assign(body.begin(), body.end());
  detail::HttpRequest request(base_, token_, ca_, std::string(method), std::string(path), std::move(payload),
                              options);
  for (std::size_t retries = 0;; ++retries) {
    try {
      return request.perform();
    } catch (...) {
      auto delay = retry ? detail::retry_delay(std::current_exception(), retries, *retry) : std::nullopt;
      if (!delay)
        throw;
      detail::wait_until(std::chrono::steady_clock::now() + *delay, options.stop);
      request.restart();
    }
  }
}
void TransferOptions::validate() const {
  require(max_requests >= 1 && max_requests <= 32, "transfer requests must be in 1..32");
  require(max_in_flight_bytes >= max_object_size + 4096 && max_in_flight_bytes <= 1024ULL * 1024 * 1024,
          "transfer buffer must fit a maximum object and response and be at most 1 GiB");
  require(max_retries <= 8, "transfer retries must be in 0..8");
  require(retry_delay_ms >= 1 && retry_delay_ms <= 30000, "transfer retry delay must be in 1..30000 ms");
  for (auto rate : {upload_bytes_per_second, download_bytes_per_second})
    require(rate == 0 || (rate >= max_requests && rate <= (1ULL << 40)),
            "transfer rate must be zero or between the request count and 1 TiB/s");
}
Refs Remote::refs(OperationOptions options, TransferOptions limits) const {
  limits.validate();
  return decode_refs(parse_json(request("GET", "v1/refs", {}, std::move(options), &limits)));
}
Json Remote::push(const Store &store, const ObjectKey &root, std::string_view name, OperationOptions options,
                  TransferOptions limits) const {
  limits.validate();
  throw_if_cancelled(options.stop);
  validate_ref(name);
  require(name.find('/') == name.npos, "remote reference must be a single component");
  auto remote = refs(options, limits);
  Json expected = remote.contains(std::string(name)) ? key_json(remote.at(std::string(name))) : Json(nullptr);
  auto closure = store.closure({root}, true, options.stop, options.progress);
  std::vector<ObjectKey> objects(closure.begin(), closure.end());
  std::size_t sent = 0, reused = 0;
  std::uint64_t total = 0;
  detail::TransferStats stats;
  detail::TrafficBudget traffic(limits);
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
    auto missing = parse_json(request("POST", "v1/missing", bytes(body), options, &limits));
    require(missing.is_array(), "invalid missing response");
    std::set<ObjectKey> unique;
    for (const auto &value : missing) {
      auto key = parse_key(value);
      require(allowed.contains(key), "server requested an unannounced object");
      unique.insert(key);
    }
    reused += end - start - unique.size();
    auto current = unique.begin();
    detail::transfer_objects(
        base_, token_, ca_, true, limits, options, stats, traffic,
        [&]() -> std::optional<ObjectKey> {
          return current == unique.end() ? std::nullopt : std::optional(*current++);
        },
        [&](const ObjectKey &key) { return store.get(key); },
        [&](const ObjectKey &, Bytes, std::size_t uploaded) {
          ++sent;
          total += uploaded;
        });
  }
  auto body = Json{{"root", key_json(root)}, {"expected", expected}}.dump();
  report_progress(options, {ProgressPhase::publishing, name});
  // The final CAS is a commit point. Once dispatched, finish receiving its
  // result instead of aborting a request the server may already have applied.
  try {
    (void)request("PUT", "v1/refs/" + std::string(name), bytes(body));
  } catch (const detail::HttpStatusError &error) {
    if (error.status != 401 && error.status != 403 && error.status != 404 && error.status != 409)
      throw PublicationUncertain(
          "remote reference publication outcome is unknown; inspect the remote reference before retrying");
    throw;
  } catch (...) {
    throw PublicationUncertain(
        "remote reference publication outcome is unknown; inspect the remote reference before retrying");
  }
  return Json{{"transferred_objects", sent},
              {"transferred_bytes", total},
              {"reused_objects", reused},
              {"peak_object_requests", stats.peak_requests},
              {"peak_object_buffer_bytes", stats.peak_buffered_bytes},
              {"object_retries", stats.retries}};
}
Json Remote::pull(const Store &store, const ObjectKey &root, std::string_view name, OperationOptions options,
                  TransferOptions limits) const {
  limits.validate();
  throw_if_cancelled(options.stop);
  validate_ref(name);
  std::vector<ObjectKey> pending{root};
  std::set<ObjectKey> seen;
  std::size_t sent = 0, reused = 0;
  std::uint64_t total = 0;
  detail::TransferStats stats;
  detail::TrafficBudget traffic(limits);
  detail::transfer_objects(
      base_, token_, ca_, false, limits, options, stats, traffic,
      [&]() -> std::optional<ObjectKey> {
        while (!pending.empty()) {
          throw_if_cancelled(options.stop);
          auto key = std::move(pending.back());
          pending.pop_back();
          if (!seen.insert(key).second)
            continue;
          Bytes data;
          try {
            data = store.get(key);
          } catch (const MissingObject &) {
            return key;
          }
          ++reused;
          auto children = dependencies(key.kind, data);
          pending.insert(pending.end(), children.begin(), children.end());
        }
        return {};
      },
      {},
      [&](const ObjectKey &key, Bytes data, std::size_t) {
        throw_if_cancelled(options.stop);
        store.import(key, data);
        ++sent;
        total += data.size();
        auto children = dependencies(key.kind, data);
        pending.insert(pending.end(), children.begin(), children.end());
      });
  report_progress(options, {ProgressPhase::publishing, name});
  store.set_ref(name, root, options.stop);
  return Json{{"transferred_objects", sent},
              {"transferred_bytes", total},
              {"reused_objects", reused},
              {"peak_object_requests", stats.peak_requests},
              {"peak_object_buffer_bytes", stats.peak_buffered_bytes},
              {"object_retries", stats.retries}};
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
