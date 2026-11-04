#pragma once
#include "pack_storage.hpp"
#include <blake3.h>

namespace index_fixture {
using namespace syrnck;
// Ordered synthetic IDs and opaque 5-byte envelopes. This fixture exercises
// index lookup/ranges only; its envelopes are deliberately not Zstd objects.
inline IndexKey key(std::uint64_t value, Kind kind = Kind::chunk) {
  IndexKey result;
  result.value[0] = static_cast<std::uint8_t>(kind);
  for (std::size_t i = 0; i < 8; ++i)
    result.value[1 + i] = static_cast<std::uint8_t>(value >> (8 * (7 - i)));
  auto state = value + 0x123456789abcdefULL;
  for (std::size_t i = 9; i < result.value.size(); ++i) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    result.value[i] = static_cast<std::uint8_t>(state);
  }
  return result;
}
inline void append(Bytes &out, std::uint64_t value, std::size_t width) {
  for (std::size_t i = 0; i < width; ++i)
    out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}
inline Json create(const fs::path &root, std::size_t count, std::uint64_t begin = 0, std::uint64_t stride = 2,
                   Kind kind = Kind::chunk) {
  require(count > 0 && count <= PackStorage::max_entries && stride > 0, "invalid index fixture");
  fs::create_directories(root / "packs");
  auto name = hex(random_bytes(16));
  auto pack = root / "packs" / path_from_utf8("pack-" + name + ".syp");
  auto index = root / "packs" / path_from_utf8("pack-" + name + ".syi");
  atomic_stream(pack, [&](std::ostream &out) {
    out.write("SYRPACK1", 8);
    for (std::size_t i = 0; i < count; ++i)
      out.write("SYR1\0", 5);
  });
  blake3_hasher hasher;
  blake3_hasher_init(&hasher);
  atomic_stream(index, [&](std::ostream &out) {
    auto write = [&](const Bytes &data) {
      out.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
      require(out.good(), "fixture write failed");
      blake3_hasher_update(&hasher, data.data(), data.size());
    };
    Bytes header{'S', 'Y', 'R', 'I', 'D', 'X', '0', '1'};
    append(header, count, 8);
    write(header);
    Bytes page;
    page.reserve(PackIndex::page_bytes);
    for (std::size_t i = 0; i < count; ++i) {
      auto raw = key(begin + i * stride, kind);
      page.insert(page.end(), raw.value.begin(), raw.value.end());
      append(page, 8 + 5 * i, 8);
      append(page, 5, 4);
      if (page.size() == PackIndex::page_bytes) {
        write(page);
        page.clear();
      }
    }
    write(page);
  });
  Key sum{};
  blake3_hasher_finalize(&hasher, sum.data(), sum.size());
  return Json{{"name", name}, {"objects", count}, {"bytes", 8 + 5 * count}, {"index_hash", hex(sum)}};
}
inline fs::path path(const fs::path &root, const Json &metadata, std::string_view extension = ".syi") {
  return root / "packs" /
         path_from_utf8("pack-" + metadata["name"].get<std::string>() + std::string(extension));
}
} // namespace index_fixture
