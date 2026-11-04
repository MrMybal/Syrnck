#include "object_compression.hpp"
#include <zstd.h>
#include <iostream>
using namespace syrnck;
using namespace syrnck::net::detail;
namespace {
void check(bool value) {
  require(value, "network compression test failed");
}
template <class F> void fails(F call) {
  bool failed = false;
  try {
    call();
  } catch (const Error &) {
    failed = true;
  }
  check(failed);
}
Bytes decode(const Bytes &encoded, std::size_t step) {
  Bytes result;
  ObjectDecoder decoder;
  for (std::size_t i = 0; i < encoded.size(); i += step)
    decoder.append(result, std::span(encoded).subspan(i, std::min(step, encoded.size() - i)));
  decoder.finish();
  return result;
}
Bytes frame(std::span<const std::uint8_t> input, bool known_size = true) {
  std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)> ctx(ZSTD_createCCtx(), &ZSTD_freeCCtx);
  check(ctx != nullptr);
  check(!ZSTD_isError(ZSTD_CCtx_setParameter(ctx.get(), ZSTD_c_contentSizeFlag, known_size ? 1 : 0)));
  Bytes result(ZSTD_compressBound(input.size()));
  auto size = ZSTD_compress2(ctx.get(), result.data(), result.size(), input.data(), input.size());
  check(!ZSTD_isError(size));
  result.resize(size);
  return result;
}
} // namespace
int main() {
  try {
    for (auto size : {0U, 1U, 1024U, 65536U, static_cast<unsigned>(max_object_size)}) {
      Bytes data(size, 0x39);
      auto encoded = frame(data);
      for (auto step : {1U, 17U, 65536U})
        check(decode(encoded, step) == data);
      encoded.pop_back();
      fails([&] { decode(encoded, 1); });
    }
    Bytes data(65536, 0x39);
    auto encoded = compress_object(data, compression_capacity(data.size(), data.size()));
    check(!encoded.empty() && encoded.size() + 64 <= data.size());
    check(decode(encoded, 11) == data);
    check(compress_object(data, 1).empty());
    check(compression_capacity(1023, 1023) == 0);
    check(compression_capacity(65536, 128) == 128);
    auto noise = random_bytes(65536);
    check(compress_object(noise, compression_capacity(noise.size(), noise.size())).empty());
    auto trailing = encoded;
    trailing.insert(trailing.end(), encoded.begin(), encoded.end());
    fails([&] { decode(trailing, trailing.size()); });
    fails([&] { decode(trailing, 1); });
    fails([&] { decode(frame(data, false), 1); });
    fails([&] { decode(frame(Bytes(max_object_size + 1, 0)), 64); });
    fails([&] { decode(Bytes{0x50, 0x2a, 0x4d, 0x18, 0, 0, 0, 0}, 1); });
    fails([&] { decode(Bytes{0x28, 0xb5, 0x2f, 0xfd, 0x21, 1, 0, 1, 0, 0}, 1); });
    // A small declared content size cannot authorize a window above 16 MiB.
    fails([&] { decode(Bytes{0x28, 0xb5, 0x2f, 0xfd, 0x80, 0x78, 1, 0, 0, 0}, 1); });
    // Declared empty content with a nonempty raw block must not overrun output.
    fails([&] { decode(Bytes{0x28, 0xb5, 0x2f, 0xfd, 0x20, 0, 9, 0, 0, 0x39}, 1); });
    fails([&] { decode(Bytes{1, 2, 3, 4, 5}, 1); });
    fails([&] { decode(Bytes{}, 1); });
    std::stop_source stop;
    stop.request_stop();
    fails([&] { compress_object(data, 4096, stop.get_token()); });
    fails([&] {
      Bytes out;
      ObjectDecoder decoder;
      decoder.append(out, encoded, stop.get_token());
    });
    std::cout << "Network compression tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
