#pragma once
#include "internal.hpp"
#include <memory>

namespace syrnck::net::detail {
// Returns an empty buffer when a frame cannot save at least 64 bytes within
// the supplied staging credit. Codec contexts are separate from body credits.
std::size_t compression_capacity(std::size_t logical_size, std::size_t available);
Bytes compress_object(std::span<const std::uint8_t>, std::size_t capacity, std::stop_token = {});
class ObjectDecoder {
public:
  ObjectDecoder();
  ~ObjectDecoder();
  // A single frame, known content size and window <= 16 MiB, no dictionary.
  // Writes directly into the decoded response; no encoded body is retained.
  void append(Bytes &, std::span<const std::uint8_t>, std::stop_token = {});
  void finish() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace syrnck::net::detail
