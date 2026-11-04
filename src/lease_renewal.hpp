#pragma once
#include "http_transport.hpp"
#include <future>
#include <thread>
namespace syrnck::net::detail {
using LeaseRequest =
    std::function<Bytes(std::string_view, std::string_view, std::span<const std::uint8_t>, OperationOptions)>;
struct LeaseRenewal {
  std::promise<Bytes> promise;
  std::future<Bytes> future = promise.get_future();
  std::string fence;
  // Destroyed first: cancel and join before destroying the promise or captured request state.
  std::jthread worker;
  LeaseRenewal(LeaseRequest request, std::string path, std::string body, std::string generation,
               std::stop_token external)
      : fence(std::move(generation)), worker([this, request = std::move(request), path = std::move(path),
                                              body = std::move(body), external](std::stop_token internal) {
          std::stop_source combined;
          std::stop_callback outer(external, [&] { combined.request_stop(); });
          std::stop_callback inner(internal, [&] { combined.request_stop(); });
          try {
            promise.set_value(request("PUT", path, bytes(body), {combined.get_token(), {}}));
          } catch (...) {
            promise.set_exception(std::current_exception());
          }
        }) {}
};
} // namespace syrnck::net::detail
