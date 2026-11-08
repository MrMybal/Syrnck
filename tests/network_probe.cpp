#include <syrnck/net.hpp>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
using namespace syrnck;
int entry(int argc, char **argv) {
  try {
    require(argc == 11,
            "network probe expects mode operation store URL kind ID ref encryption requests buffer-MiB");
    std::string mode = argv[1], operation = argv[2];
    std::optional<Key> key;
    if (std::string_view(argv[8]) == "encrypted") {
      key.emplace();
      key->fill(0x39);
    }
    Store store(path_from_utf8(argv[3]), key);
    net::Remote remote(argv[4], "syrnck-test-token-0123456789-0123456789");
    ObjectKey root{parse_kind(argv[5]), argv[6]};
    if (mode == "invalid") {
      auto manifest = store.metadata(Kind::manifest, root.id);
      manifest["file_hash"] = std::string(64, '0');
      auto value = manifest.dump();
      root = store.put(Kind::manifest, {reinterpret_cast<const std::uint8_t *>(value.data()), value.size()})
                 .first;
    }
    std::stop_source stop;
    std::jthread late;
    auto caller = std::this_thread::get_id();
    std::size_t events = 0, completed = 0, publication_events = 0;
    bool bytes_seen = false;
    OperationOptions options{stop.get_token(), [&](const ProgressEvent &event) {
                               require(caller == std::this_thread::get_id(),
                                       "network observer changed thread");
                               ++events;
                               if (event.phase == ProgressPhase::transferring) {
                                 completed += event.complete;
                                 bytes_seen |= event.bytes > 0;
                                 if (event.path.starts_with("v1/objects/chunk/") && event.bytes > 0) {
                                   if (mode == "renew_stop" && !late.joinable())
                                     late = std::jthread([&] {
                                       if (const auto *signal = std::getenv("SYRNCK_TEST_RENEWAL_READY")) {
                                         const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                                         while (!fs::exists(path_from_utf8(signal)) && std::chrono::steady_clock::now() < deadline)
                                           std::this_thread::sleep_for(std::chrono::milliseconds(10));
                                       } else {
                                         std::this_thread::sleep_for(std::chrono::milliseconds(500));
                                       }
                                       stop.request_stop();
                                     });
                                   if ((mode == "backoff" || mode == "pace") && !late.joinable())
                                     late = std::jthread([&] {
                                       std::this_thread::sleep_for(std::chrono::milliseconds(150));
                                       stop.request_stop();
                                     });
                                   if (mode == "transfer")
                                     stop.request_stop();
                                   if (mode == "throw")
                                     throw Error("network observer sentinel");
                                 }
                               }
                               if (event.phase == ProgressPhase::publishing) {
                                 if (mode == "publish")
                                   stop.request_stop();
                                 if (mode == "late")
                                   late = std::jthread([&] {
                                     std::this_thread::sleep_for(std::chrono::milliseconds(150));
                                     stop.request_stop();
                                   });
                               }
                             }};
    if (mode == "pre")
      stop.request_stop();
    net::TransferOptions limits{static_cast<std::size_t>(std::stoull(argv[9])),
                                static_cast<std::size_t>(std::stoull(argv[10]) * 1024 * 1024)};
    if (mode == "backoff")
      limits.retry_delay_ms = 2000;
    if (mode == "pace") {
      limits.upload_bytes_per_second = 1024;
      limits.download_bytes_per_second = 1024;
    }
    if (mode == "renew" || mode == "renew_stop") {
      // These tests exercise lease timing independently of compression gains.
      limits.compression = net::ObjectCompression::disabled;
      if (operation == "push") {
        limits.upload_bytes_per_second = 128 * 1024;
        limits.require_upload_lease = true;
      } else {
        limits.download_bytes_per_second = 128 * 1024;
        limits.require_read_lease = true;
      }
    }
    try {
      Json result;
      if (mode.starts_with("record")) {
        net::PublicationObserver observer = [&](const ObjectKey &published) {
          require(caller == std::this_thread::get_id() && published == root, "publication observer received another thread or root");
          ++publication_events;
          const auto session = store.transfer_session(TransferSession::identity("push", remote.endpoint_fingerprint(), root, argv[7]));
          require(session && (session->state == "publishing" || session->state == "uncertain" || session->state == "complete"), "publication observer ran before a durable publication marker");
          store.set_ref("publication-proof", root);
          if (mode == "record-fail") throw Error("publication bookkeeping sentinel");
          if (mode == "record-stop" || mode == "record-resume-stop") stop.request_stop();
        };
        if (mode == "record-resume" || mode == "record-resume-stop")
          result = remote.resume(store, TransferSession::identity("push", remote.endpoint_fingerprint(), root, argv[7]), options, limits, observer);
        else result = remote.push_expected(store, root, argv[7], {}, options, limits, observer);
      } else if (operation == "push")
        result = remote.push(store, root, argv[7], options, limits);
      else if (operation == "pull")
        result = remote.pull(store, root, argv[7], options, limits);
      else {
        (void)remote.refs(options);
        result = Json::object();
      }
      require(mode == "ok" || mode == "late" || mode == "renew" || mode == "record-resume" || mode == "record-stop" || mode == "record-resume-stop", "network probe expected failure");
      std::cout << Json{{"result", result},
                        {"events", events},
                        {"completed", completed},
                        {"bytes_seen", bytes_seen},
                        {"stopped", stop.stop_requested()}, {"publication_events", publication_events}}
                       .dump()
                << '\n';
    } catch (const net::PublicationUncertain &) {
      require(mode == "uncertain" || mode == "record-fail", "unexpected uncertain publication");
      std::cout << "{\"uncertain\":true}\n";
    } catch (const Cancelled &) {
      require(mode == "pre" || mode == "transfer" || mode == "publish" || mode == "backoff" ||
                  mode == "pace" || mode == "renew_stop",
              "unexpected cancellation");
      std::cout << Json{{"cancelled", true}, {"events", events}}.dump() << '\n';
    } catch (const Error &error) {
      if (mode == "overlong" && std::string_view(error.what()).starts_with("HTTP transfer failed: ")) {
        std::cout << "{\"oversized_response\":true}\n";
        return 0;
      }
      if (mode == "corrupt" && std::string_view(error.what()) == "object hash mismatch") {
        std::cout << "{\"corrupt_response\":true}\n";
        return 0;
      }
      if (mode == "invalid" && std::string_view(error.what()) == "reconstructed file hash mismatch") {
        std::cout << "{\"invalid_source\":true}\n";
        return 0;
      }
      if (mode != "throw")
        throw; // Preserve the real transport/lease diagnostic for unexpected failures.
      require(std::string_view(error.what()) == "network observer sentinel",
              "network observer error was lost");
      std::cout << "{\"observer_error\":true}\n";
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
  std::vector<std::string> values;
  for (int i = 0; i < argc; ++i)
    values.push_back(utf8(argv[i]));
  std::vector<char *> pointers;
  for (auto &value : values)
    pointers.push_back(value.data());
  return entry(argc, pointers.data());
}
#else
int main(int argc, char **argv) {
  return entry(argc, argv);
}
#endif
