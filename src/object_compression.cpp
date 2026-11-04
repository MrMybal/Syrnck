#include "object_compression.hpp"
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#include <zstd_errors.h>
#include <algorithm>

namespace syrnck::net::detail {
std::size_t compression_capacity(std::size_t size, std::size_t available) {
  return size >= 1024 && available >= 64 ? std::min(size - 64, available) : 0;
}
Bytes compress_object(std::span<const std::uint8_t> data, std::size_t capacity, std::stop_token stop) {
  throw_if_cancelled(stop);
  if (!capacity)
    return {};
  require(data.size() >= 64 && data.size() <= max_object_size && capacity <= data.size() - 64,
          "invalid network compression capacity");
  Bytes encoded(capacity);
  auto size = ZSTD_compress(encoded.data(), encoded.size(), data.data(), data.size(), 1);
  throw_if_cancelled(stop);
  if (ZSTD_isError(size) && ZSTD_getErrorCode(size) == ZSTD_error_dstSize_tooSmall)
    return {};
  require(!ZSTD_isError(size), "network compression failed");
  encoded.resize(size);
  return encoded;
}
struct ObjectDecoder::Impl {
  std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> context{nullptr, &ZSTD_freeDCtx};
  std::array<std::uint8_t, ZSTD_FRAMEHEADERSIZE_MAX> header{};
  std::size_t header_size = 0, decoded = 0, expected = 0;
  bool initialized = false, ended = false;
  void decode(Bytes &out, std::span<const std::uint8_t> data, std::stop_token stop) {
    ZSTD_inBuffer input{data.data(), data.size(), 0};
    while (input.pos < input.size) {
      throw_if_cancelled(stop);
      require(!ended, "trailing network compressed data");
      ZSTD_outBuffer output{out.data(), out.size(), decoded};
      auto previous = input.pos;
      auto result = ZSTD_decompressStream(context.get(), &output, &input);
      require(!ZSTD_isError(result), "invalid network compressed object");
      require(input.pos != previous || output.pos != decoded, "oversized network compressed object");
      decoded = output.pos;
      ended = result == 0;
    }
  }
};
ObjectDecoder::ObjectDecoder() : impl_(std::make_unique<Impl>()) {}
ObjectDecoder::~ObjectDecoder() = default;
void ObjectDecoder::append(Bytes &out, std::span<const std::uint8_t> data, std::stop_token stop) {
  auto &state = *impl_;
  throw_if_cancelled(stop);
  while (!state.initialized && !data.empty()) {
    require(state.header_size < state.header.size(), "invalid network compression frame");
    state.header[state.header_size++] = data.front();
    data = data.subspan(1);
    ZSTD_FrameHeader frame{};
    auto needed = ZSTD_getFrameHeader(&frame, state.header.data(), state.header_size);
    require(!ZSTD_isError(needed), "invalid network compression frame");
    if (needed)
      continue;
    require(frame.frameType == ZSTD_frame && frame.dictID == 0 &&
                frame.frameContentSize != ZSTD_CONTENTSIZE_UNKNOWN &&
                frame.frameContentSize <= max_object_size && frame.windowSize <= max_object_size,
            "unsupported network compression frame");
    state.expected = static_cast<std::size_t>(frame.frameContentSize);
    state.context.reset(ZSTD_createDCtx());
    require(state.context != nullptr &&
                !ZSTD_isError(ZSTD_DCtx_setParameter(state.context.get(), ZSTD_d_windowLogMax, 24)),
            "cannot create network decompressor");
    out.resize(state.expected);
    state.initialized = true;
    state.decode(out, std::span(state.header).first(state.header_size), stop);
  }
  if (state.initialized)
    state.decode(out, data, stop);
}
void ObjectDecoder::finish() const {
  require(impl_->initialized && impl_->ended && impl_->decoded == impl_->expected,
          "incomplete network compressed object");
}
} // namespace syrnck::net::detail
