#include "internal.hpp"
#include "tables.hpp"
#include <algorithm>
#include <cmath>
namespace syrnck {
// Port of fastcdc 5.0.0 v2020::cut_gear, normalization level 1, seed 0.
std::size_t chunk_cut(std::span<const std::uint8_t> source, const Chunking &c) {
  auto remaining = std::min(source.size(), static_cast<std::size_t>(c.max));
  if (c.fixed || remaining <= c.min)
    return remaining;
  auto center = std::min(remaining, static_cast<std::size_t>(c.avg));
  auto bits = static_cast<std::size_t>(std::round(std::log2(c.avg)));
  auto mask_s = masks[bits + 1], mask_l = masks[bits - 1];
  auto index = static_cast<std::size_t>(c.min / 2);
  std::uint64_t h = 0;
  auto scan = [&](std::size_t limit, std::uint64_t mask) -> std::size_t {
    while (index < limit) {
      auto a = index * 2;
      h = (h << 2) + (gear[source[a]] << 1);
      if ((h & (mask << 1)) == 0)
        return a;
      h += gear[source[a + 1]];
      if ((h & mask) == 0)
        return a + 1;
      ++index;
    }
    return 0;
  };
  if (auto n = scan(center / 2, mask_s))
    return n;
  if (auto n = scan(remaining / 2, mask_l))
    return n;
  return remaining;
}
} // namespace syrnck
