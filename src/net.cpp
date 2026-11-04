#include "internal.hpp"
#include "http_transport.hpp"
#include "object_compression.hpp"
#include "upload_transport.hpp"
#include "read_transport.hpp"
#include <curl/curl.h>
#include <exception>
#include <httplib.h>
#include <iostream>
#include <monocypher.h>
#include <mutex>
#include <limits>
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
std::optional<ObjectKey> reference(const Refs &refs, std::string_view name) {
  auto found = refs.find(std::string(name));
  return found == refs.end() ? std::nullopt : std::optional(found->second);
}
std::pair<TransferSession, bool> session_for(const Store &store, std::string_view direction,
                                             std::string_view endpoint, const ObjectKey &root,
                                             std::string_view name, std::optional<ObjectKey> expected) {
  auto id = TransferSession::identity(direction, endpoint, root, name);
  auto previous = store.transfer_session(id);
  if (previous && previous->state != "complete")
    return {std::move(*previous), false};
  TransferSession session;
  session.id = std::move(id);
  session.direction = direction;
  session.endpoint = endpoint;
  session.reference = name;
  session.root = root;
  session.expected = std::move(expected);
  session.validate();
  return {std::move(session), true};
}
Json transfer_result(const TransferSession &session, std::size_t sent = 0, std::uint64_t total = 0,
                     std::size_t reused = 0, const detail::TransferStats &stats = {}, bool reconciled = false,
                     const detail::TrafficBudget *traffic = nullptr) {
  return Json{{"transferred_objects", sent},
              {"transferred_bytes", total},
              {"reused_objects", reused},
              {"peak_object_requests", stats.peak_requests},
              {"peak_object_buffer_bytes", stats.peak_buffered_bytes},
              {"object_retries", stats.retries},
              {"object_upload_bytes", traffic ? traffic->uploaded_bytes() : 0},
              {"object_download_bytes", traffic ? traffic->downloaded_bytes() : 0},
              {"session", session.id},
              {"reconciled", reconciled},
              {"upload_lease", session.upload_lease ? Json(*session.upload_lease) : Json(nullptr)},
              {"read_lease", session.read_lease ? Json(*session.read_lease) : Json(nullptr)}};
}
void save_failure(const Store &store, TransferSession &session, std::string_view state) noexcept {
  // The durable 'publishing' record already protects an unknown outcome.
  // A secondary journal error must not hide the original request failure.
  try {
    session.state = state;
    store.write_transfer_session(session);
  } catch (...) {
  }
}
Json reconciled(const Store &store, TransferSession &session, const OperationOptions &options) {
  report_progress(options, {ProgressPhase::publishing, session.reference});
  session.state = "complete";
  store.write_transfer_session(session, options.stop);
  return transfer_result(session, 0, 0, 0, {}, true);
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
  detail::HttpRequest request(
      base_, token_, ca_, std::string(method), std::string(path), std::move(payload), options,
      path == "v1/capabilities" || path.starts_with("v1/uploads/") || path.starts_with("v1/reads/")
          ? 16384
          : max_object_size);
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
  require(compression == ObjectCompression::automatic || compression == ObjectCompression::disabled ||
              compression == ObjectCompression::zstd,
          "invalid network compression mode");
  require(retry_delay_ms >= 1 && retry_delay_ms <= 30000, "transfer retry delay must be in 1..30000 ms");
  for (auto rate : {upload_bytes_per_second, download_bytes_per_second})
    require(rate == 0 || (rate >= max_requests && rate <= (1ULL << 40)),
            "transfer rate must be zero or between the request count and 1 TiB/s");
}
bool Remote::negotiate_compression(std::optional<bool> &cached, OperationOptions operation,
                                   const TransferOptions &limits) const {
  if (limits.compression == ObjectCompression::disabled)
    return false;
  if (!cached) {
    bool supported = false;
    try {
      auto caps = parse_json(request("GET", "v1/capabilities", {}, std::move(operation), &limits));
      require(caps.is_object(), "invalid server capabilities");
      if (caps.contains("object_zstd")) {
        require(caps["object_zstd"].is_number_unsigned() && caps["object_zstd"].get<std::uint64_t>() <= 1,
                "invalid server capabilities");
        supported = caps["object_zstd"] == 1;
      }
    } catch (const detail::HttpStatusError &error) {
      if (error.status != 404)
        throw;
    }
    cached = supported;
  }
  require(*cached || limits.compression != ObjectCompression::zstd,
          "server does not support network Zstandard compression");
  return *cached;
}
Refs Remote::refs(OperationOptions options, TransferOptions limits) const {
  limits.validate();
  return decode_refs(parse_json(request("GET", "v1/refs", {}, std::move(options), &limits)));
}
void ServeOptions::validate() const {
  require(upload_lease_seconds >= 1 && upload_lease_seconds <= 86400,
          "upload lease duration must be in 1..86400 seconds");
  require(read_lease_seconds >= 1 && read_lease_seconds <= 86400,
          "read lease duration must be in 1..86400 seconds");
}
Json Remote::push(const Store &store, const ObjectKey &root, std::string_view name, OperationOptions options,
                  TransferOptions limits) const {
  limits.validate();
  require(!limits.require_read_lease, "read lease requirement is only available for pull");
  throw_if_cancelled(options.stop);
  validate_ref(name);
  require(name.find('/') == name.npos, "remote reference must be a single component");
  auto remote = refs(options, limits);
  auto actual = reference(remote, name);
  auto [session, fresh] = session_for(store, "push", hash(bytes(base_)), root, name, actual);
  detail::UploadProtection protection(
      store, session, options, limits,
      [&](std::string_view method, std::string_view path, std::span<const std::uint8_t> body,
          OperationOptions control) { return request(method, path, body, std::move(control), &limits); });
  if (!fresh) {
    if (protection.receipt())
      return reconciled(store, session, options);
    if (session.state == "publishing" || session.state == "uncertain" || session.state == "conflict") {
      if (actual == root) {
        protection.confirm();
        return reconciled(store, session, options);
      }
      if (session.state != "conflict") {
        save_failure(store, session, "uncertain");
        throw PublicationUncertain(
            "remote reference publication outcome is unknown; inspect the remote reference before retrying");
      }
    }
    if (session.state == "conflict" || actual != session.expected) {
      save_failure(store, session, "conflict");
      throw Error("transfer reference changed; discard the session before restarting");
    }
  }
  auto closure = store.closure({root}, true, options.stop, options.progress);
  std::vector<ObjectKey> objects(closure.begin(), closure.end());
  if (fresh)
    store.write_transfer_session(session, options.stop);
  protection.acquire();
  if (protection.published())
    return reconciled(store, session, options);
  std::size_t sent = 0, reused = 0;
  std::uint64_t total = 0;
  detail::TransferStats stats;
  detail::TrafficBudget traffic(limits);
  std::optional<bool> compressed;
  for (std::size_t start = 0; start < objects.size(); start += 4096) {
    throw_if_cancelled(options.stop);
    protection.tick();
    auto end = std::min(objects.size(), start + 4096);
    Json batch = Json::array();
    std::set<ObjectKey> allowed;
    for (auto i = start; i < end; ++i) {
      batch.push_back(key_json(objects[i]));
      allowed.insert(objects[i]);
    }
    require(start / 4096 <= std::numeric_limits<std::uint32_t>::max(),
            "upload inventory page limit exceeded");
    protection.announce(static_cast<std::uint32_t>(start / 4096), batch);
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
        },
        [&] { protection.tick(); }, [&] { return negotiate_compression(compressed, options, limits); });
  }
  protection.finish();
  auto publication = Json{{"root", key_json(root)},
                          {"expected", session.expected ? key_json(*session.expected) : Json(nullptr)}};
  if (!protection.publication().is_null())
    publication["upload"] = protection.publication();
  auto body = publication.dump();
  report_progress(options, {ProgressPhase::publishing, name});
  session.state = "publishing";
  store.write_transfer_session(session, options.stop);
  // Committing the publishing marker ends cancellation. Finish dispatch and
  // receipt even if a stop arrives; a crash in this gap must reconcile the CAS.
  try {
    (void)request("PUT", "v1/refs/" + std::string(name), bytes(body));
  } catch (const detail::HttpStatusError &error) {
    const bool lease_rejection =
        !protection.publication().is_null() && (error.status == 412 || error.status == 428);
    if (error.status != 401 && error.status != 403 && error.status != 404 && error.status != 409 &&
        !lease_rejection) {
      save_failure(store, session, "uncertain");
      throw PublicationUncertain(
          "remote reference publication outcome is unknown; inspect the remote reference before retrying");
    }
    save_failure(store, session, error.status == 409 ? "conflict" : "transferring");
    throw;
  } catch (...) {
    save_failure(store, session, "uncertain");
    throw PublicationUncertain(
        "remote reference publication outcome is unknown; inspect the remote reference before retrying");
  }
  session.state = "complete";
  try {
    store.write_transfer_session(session);
  } catch (...) {
    throw PublicationUncertain(
        "transfer published but session completion could not be saved; resume the session");
  }
  return transfer_result(session, sent, total, reused, stats, false, &traffic);
}
Json Remote::pull(const Store &store, const ObjectKey &root, std::string_view name, OperationOptions options,
                  TransferOptions limits) const {
  limits.validate();
  require(!limits.require_upload_lease, "upload lease requirement is only available for push");
  throw_if_cancelled(options.stop);
  validate_ref(name);
  auto actual = reference(store.refs(), name);
  auto [session, fresh] = session_for(store, "pull", hash(bytes(base_)), root, name, actual);
  detail::ReadProtection protection(
      store, session, options, limits,
      [&](std::string_view method, std::string_view path, std::span<const std::uint8_t> body,
          OperationOptions control) { return request(method, path, body, std::move(control), &limits); });
  if (!fresh) {
    if ((session.state != "transferring" || actual != session.expected) && actual == root) {
      (void)store.closure({root}, true, options.stop, options.progress);
      protection.release();
      return reconciled(store, session, options);
    }
    if (session.state == "conflict" || actual != session.expected) {
      save_failure(store, session, "conflict");
      throw Error("transfer reference changed; discard the session before restarting");
    }
  }
  session.state = "transferring";
  store.write_transfer_session(session, options.stop);
  std::vector<ObjectKey> pending{root};
  std::set<ObjectKey> seen;
  std::size_t sent = 0, reused = 0;
  std::uint64_t total = 0;
  detail::TransferStats stats;
  detail::TrafficBudget traffic(limits);
  std::optional<bool> compressed;
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
            protection.acquire();
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
      },
      [&] { protection.tick(); }, [&] { return negotiate_compression(compressed, options, limits); });
  protection.release();
  report_progress(options, {ProgressPhase::publishing, name});
  session.state = "publishing";
  store.write_transfer_session(session, options.stop);
  if (!store.compare_set_ref(name, session.expected, root, options.stop)) {
    save_failure(store, session, "conflict");
    throw Error("transfer reference changed; discard the session before restarting");
  }
  session.state = "complete";
  try {
    store.write_transfer_session(session);
  } catch (...) {
    throw Error("transfer published but session completion could not be saved; resume the session");
  }
  return transfer_result(session, sent, total, reused, stats, false, &traffic);
}
Json Remote::resume(const Store &store, std::string_view id, OperationOptions options,
                    TransferOptions limits) const {
  limits.validate();
  throw_if_cancelled(options.stop);
  auto session = store.transfer_session(id);
  require(session.has_value(), "unknown transfer session");
  require(session->endpoint == hash(bytes(base_)), "transfer session belongs to a different endpoint");
  if (session->state == "complete")
    return transfer_result(*session, 0, 0, 0, {}, true);
  if (session->direction == "push")
    return push(store, session->root, session->reference, std::move(options), limits);
  return pull(store, session->root, session->reference, std::move(options), limits);
}
void serve(Store &store, std::string_view token, std::string_view listen, Language language,
           ServeOptions limits) {
  limits.validate();
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
  // Captured state outlives the server and its queued handlers during teardown.
  std::mutex mutex;
  auto expected = unhex(hash(bytes(token)));
  auto lease_reply = [&](const UploadLease &lease, auto &response) {
    response.set_content(Json{{"lease", lease.json()}, {"lease_seconds", limits.upload_lease_seconds}}.dump(),
                         "application/json");
  };
  httplib::Server server;
  server.new_task_queue = []() { return new httplib::ThreadPool(16); };
  server.set_payload_max_length(max_object_size);
  server.set_read_timeout(30, 0);
  server.set_write_timeout(120, 0);
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
      // The bounded body has been consumed before this handler. Rejecting a
      // coding during pre-routing can reset a connection with unread data,
      // hiding the 415 response on Windows and confusing persistent clients.
      auto count = request.get_header_value_count("Syrnck-Object-Encoding");
      auto http_count = request.get_header_value_count("Content-Encoding");
      if ((http_count && (http_count != 1 || request.get_header_value("Content-Encoding") != "identity")) ||
          (count && (count != 1 || !limits.object_compression || request.method != "PUT" ||
                     !request.path.starts_with("/v1/objects/") ||
                     request.get_header_value("Syrnck-Object-Encoding") != "zstd"))) {
        response.status = 415;
        return;
      }
      std::lock_guard lock(mutex);
      try {
        handler(request, response);
      } catch (const LeaseRejected &e) {
        response.status = 412;
        response.set_content(e.what(), "text/plain");
      } catch (const MissingObject &e) {
        response.status = 404;
        response.set_content(e.what(), "text/plain");
      } catch (const std::exception &e) {
        response.status = 400;
        response.set_content(e.what(), "text/plain");
      }
    };
  };
  server.Get("/v1/capabilities", guarded([&](const auto &, auto &response) {
               response.set_content(Json{{"upload_leases", 1},
                                         {"upload_inventories", 1},
                                         {"object_zstd", limits.object_compression ? 1 : 0},
                                         {"lease_seconds", limits.upload_lease_seconds},
                                         {"read_leases", 1},
                                         {"read_lease_seconds", limits.read_lease_seconds}}
                                        .dump(),
                                    "application/json");
             }));
  server.Get(R"(/v1/reads/([a-f0-9]{64}))", guarded([&](const auto &request, auto &response) {
               auto lease = store.read_lease(request.matches[1].str());
               if (!lease) {
                 response.status = 404;
                 return;
               }
               response.set_content(
                   Json{{"lease", lease->json()}, {"lease_seconds", limits.read_lease_seconds}}.dump(),
                   "application/json");
             }));
  server.Put(R"(/v1/reads/([a-f0-9]{64}))", guarded([&](const auto &request, auto &response) {
               require(request.body.size() <= 16384, "read lease request exceeds size limit");
               auto j = parse_json(bytes(request.body));
               require(j.is_object() && j.size() == 2 && j.contains("root") && j.contains("fence") &&
                           (j["fence"].is_null() || j["fence"].is_string()),
                       "invalid read lease request");
               std::optional<std::string> fence;
               if (!j["fence"].is_null())
                 fence = j["fence"].template get<std::string>();
               auto lease = store.acquire_read_lease(request.matches[1].str(), parse_key(j["root"]),
                                                     limits.read_lease_seconds, fence);
               response.set_content(
                   Json{{"lease", lease.json()}, {"lease_seconds", limits.read_lease_seconds}}.dump(),
                   "application/json");
             }));
  server.Post(R"(/v1/reads/([a-f0-9]{64})/release)", guarded([&](const auto &request, auto &response) {
                require(request.body.size() <= 16384, "read lease request exceeds size limit");
                auto j = parse_json(bytes(request.body));
                require(j.is_object() && j.size() == 1 && j.contains("fence") && j["fence"].is_string(),
                        "invalid read lease request");
                auto id = request.matches[1].str();
                store.release_read_lease(id, j["fence"].template get<std::string>());
                response.set_content(Json{{"lease", store.read_lease(id)->json()},
                                          {"lease_seconds", limits.read_lease_seconds}}
                                         .dump(),
                                     "application/json");
              }));
  server.Get(R"(/v1/uploads/([a-f0-9]{64}))", guarded([&](const auto &request, auto &response) {
               auto lease = store.upload_lease(request.matches[1].str());
               if (!lease) {
                 response.status = 404;
                 return;
               }
               lease_reply(*lease, response);
             }));
  server.Put(R"(/v1/uploads/([a-f0-9]{64}))", guarded([&](const auto &request, auto &response) {
               require(request.body.size() <= 16384, "upload lease request exceeds size limit");
               auto j = parse_json(bytes(request.body));
               require(j.is_object() &&
                           (j.size() == 4 || (j.size() == 5 && j.contains("inventory") &&
                                              j["inventory"].is_boolean() && j["inventory"] == true)) &&
                           j.contains("root") && j.contains("reference") && j["reference"].is_string() &&
                           j.contains("expected") && j.contains("fence") &&
                           (j["fence"].is_null() || j["fence"].is_string()),
                       "invalid upload lease request");
               std::optional<ObjectKey> expected;
               if (!j["expected"].is_null())
                 expected = parse_key(j["expected"]);
               std::optional<std::string> fence;
               if (!j["fence"].is_null())
                 fence = j["fence"].template get<std::string>();
               auto lease = store.acquire_upload_lease(
                   request.matches[1].str(), parse_key(j["root"]), j["reference"].template get<std::string>(),
                   expected, limits.upload_lease_seconds, fence, j.contains("inventory"));
               lease_reply(lease, response);
             }));
  server.Put(R"(/v1/uploads/([a-f0-9]{64})/inventory/([0-9]{1,4}))",
             guarded([&](const auto &request, auto &response) {
               require(request.body.size() <= 1024 * 1024, "upload inventory request exceeds size limit");
               auto j = parse_json(bytes(request.body));
               require(j.is_object() && j.size() == 2 && j.contains("fence") && j["fence"].is_string() &&
                           j.contains("objects") && j["objects"].is_array() && !j["objects"].empty() &&
                           j["objects"].size() <= 4096,
                       "invalid upload inventory request");
               std::vector<ObjectKey> objects;
               for (const auto &key : j["objects"])
                 objects.push_back(parse_key(key));
               store.register_upload_inventory(
                   request.matches[1].str(), j["fence"].template get<std::string>(),
                   static_cast<std::uint32_t>(std::stoul(request.matches[2].str())), objects);
               response.status = 204;
             }));
  server.Post(R"(/v1/uploads/([a-f0-9]{64})/confirm)", guarded([&](const auto &request, auto &response) {
                require(request.body.size() <= 16384, "upload lease request exceeds size limit");
                auto j = parse_json(bytes(request.body));
                require(j.is_object() && j.size() == 1 && j.contains("fence") && j["fence"].is_string(),
                        "invalid upload lease request");
                auto id = request.matches[1].str();
                store.confirm_upload_lease(id, j["fence"].template get<std::string>());
                lease_reply(*store.upload_lease(id), response);
              }));
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
               response.set_header("Vary", "Syrnck-Accept-Object-Encoding");
               if (limits.object_compression &&
                   request.get_header_value_count("Syrnck-Accept-Object-Encoding") == 1 &&
                   request.get_header_value("Syrnck-Accept-Object-Encoding") == "zstd") {
                 auto encoded =
                     detail::compress_object(data, detail::compression_capacity(data.size(), data.size()));
                 if (!encoded.empty()) {
                   data = std::move(encoded);
                   response.set_header("Syrnck-Object-Encoding", "zstd");
                 }
               }
               response.set_content(text(data), "application/octet-stream");
             }));
  server.Put(R"(/v1/objects/(chunk|manifest|revision)/([a-f0-9]{64}))",
             guarded([&](const auto &request, auto &response) {
               ObjectKey key{parse_kind(request.matches[1].str()), request.matches[2].str()};
               if (request.has_header("Syrnck-Object-Encoding")) {
                 Bytes data;
                 detail::ObjectDecoder decoder;
                 decoder.append(data, bytes(request.body));
                 decoder.finish();
                 response.status = store.import(key, data) ? 201 : 204;
               } else {
                 response.status = store.import(key, bytes(request.body)) ? 201 : 204;
               }
             }));
  server.Get("/v1/refs", guarded([&](const auto &, auto &response) {
               response.set_content(refs_json(store.refs()).dump(), "application/json");
             }));
  server.Put(R"(/v1/refs/([A-Za-z0-9._-]+))", guarded([&](const auto &request, auto &response) {
               auto name = request.matches[1].str();
               validate_ref(name);
               auto j = parse_json(bytes(request.body));
               require(j.is_object() && (j.size() == 2 || (j.size() == 3 && j.contains("upload"))) &&
                           j.contains("root") && j.contains("expected"),
                       "invalid publication request");
               auto root = parse_key(j.at("root"));
               std::optional<ObjectKey> previous;
               if (!j.at("expected").is_null())
                 previous = parse_key(j.at("expected"));
               if (j.contains("upload")) {
                 const auto &u = j["upload"];
                 require(u.is_object() && u.size() == 2 && u.contains("id") && u["id"].is_string() &&
                             u.contains("fence") && u["fence"].is_string(),
                         "invalid upload lease request");
                 auto id = u["id"].template get<std::string>();
                 auto lease = store.upload_lease(id);
                 if (!lease) {
                   response.status = 404;
                   return;
                 }
                 require(lease->root == root && lease->reference == name && lease->expected == previous,
                         "upload lease descriptor mismatch");
                 response.status =
                     store.publish_upload_lease(id, u["fence"].template get<std::string>()) ? 204 : 409;
               } else if (limits.require_upload_lease) {
                 response.status = 428;
               } else {
                 response.status = store.compare_set_ref(name, previous, root) ? 204 : 409;
               }
             }));
  std::cerr << localize("Syrnck listening at http://", language) << host << ':' << port << '\n';
  require(server.listen(host, port), "HTTP server failed to listen");
}
} // namespace syrnck::net
