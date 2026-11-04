#include "pack_index.hpp"
#include <algorithm>
#include <blake3.h>
#include <fstream>

namespace syrnck {
namespace {
std::uint64_t integer(std::span<const std::uint8_t> data) {
  std::uint64_t result = 0;
  for (std::size_t i = 0; i < data.size(); ++i)
    result |= static_cast<std::uint64_t>(data[i]) << (8 * i);
  return result;
}
Key digest(std::span<const std::uint8_t> data) {
  blake3_hasher hasher;
  blake3_hasher_init(&hasher);
  blake3_hasher_update(&hasher, data.data(), data.size());
  Key result{};
  blake3_hasher_finalize(&hasher, result.data(), result.size());
  return result;
}
std::pair<std::uint64_t, std::uint64_t> filter_hashes(const IndexKey &key) {
  auto id = std::span(key.value).subspan(1);
  auto mix = [](std::uint64_t value) {
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
  };
  auto first = integer(id.first(8)) ^ integer(id.subspan(16, 8)) ^
               (static_cast<std::uint64_t>(key.value[0]) * 0x9e3779b97f4a7c15ULL);
  return {mix(first), mix(integer(id.subspan(8, 8)) ^ integer(id.subspan(24, 8))) | 1ULL};
}
void check_file(const fs::path &path, std::size_t size) {
  require(fs::is_regular_file(path) && !fs::is_symlink(path) && fs::file_size(path) == size,
          "missing, invalid or resized pack index");
}
void read_exact(std::istream &in, std::span<std::uint8_t> data) {
  in.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(data.size()));
  require(in.good() && static_cast<std::size_t>(in.gcount()) == data.size(), "pack index read failed");
}
} // namespace
IndexKey IndexKey::from(const ObjectKey &key) {
  require(key.id.size() == 64, "invalid object id");
  require(key.kind >= Kind::chunk && key.kind <= Kind::revision, "invalid object kind");
  IndexKey result;
  result.value[0] = static_cast<std::uint8_t>(key.kind);
  auto nibble = [](char c) -> std::uint8_t {
    if (c >= '0' && c <= '9')
      return static_cast<std::uint8_t>(c - '0');
    if (c >= 'a' && c <= 'f')
      return static_cast<std::uint8_t>(c - 'a' + 10);
    throw Error("invalid object id");
  };
  for (std::size_t i = 0; i < 32; ++i)
    result.value[i + 1] = static_cast<std::uint8_t>((nibble(key.id[2 * i]) << 4) | nibble(key.id[2 * i + 1]));
  return result;
}
ObjectKey IndexKey::object() const {
  return {static_cast<Kind>(value[0]), hex(std::span(value).subspan(1))};
}
IndexRecord PackIndex::record(std::span<const std::uint8_t> data, std::size_t ordinal) {
  require(data.size() % record_size == 0 && ordinal < data.size() / record_size, "invalid index record");
  auto raw = data.subspan(ordinal * record_size, record_size);
  IndexKey key;
  std::copy_n(raw.begin(), key.value.size(), key.value.begin());
  return {key, integer(raw.subspan(33, 8)), static_cast<std::uint32_t>(integer(raw.subspan(41, 4)))};
}
void PackIndex::add_filter(const IndexKey &key) {
  auto [position, step] = filter_hashes(key);
  const auto bits = static_cast<std::uint64_t>(filter_.size()) * 8;
  for (int i = 0; i < 7; ++i, position += step) {
    auto bit = position % bits;
    filter_[static_cast<std::size_t>(bit / 8)] |= static_cast<std::uint8_t>(1U << (bit % 8));
  }
}
bool PackIndex::may_contain(const IndexKey &key) const {
  auto [position, step] = filter_hashes(key);
  const auto bits = static_cast<std::uint64_t>(filter_.size()) * 8;
  for (int i = 0; i < 7; ++i, position += step) {
    auto bit = position % bits;
    if ((filter_[static_cast<std::size_t>(bit / 8)] & (1U << (bit % 8))) == 0)
      return false;
  }
  return true;
}
PackIndex PackIndex::scan(const fs::path &path, std::size_t count, std::uint64_t pack_bytes,
                          std::string_view expected_hash) {
  require(count > 0 && count <= 1024 * 1024, "invalid pack index count");
  check_file(path, header_size + record_size * count);
  PackIndex result;
  result.path_ = path;
  result.count_ = count;
  result.pages_.reserve((count + records_per_page - 1) / records_per_page);
  result.filter_.resize(std::max<std::size_t>(8, (count * 10 + 7) / 8));
  std::ifstream in(path, std::ios::binary);
  std::array<std::uint8_t, header_size> header{};
  read_exact(in, header);
  require(std::equal(header.begin(), header.begin() + 8, "SYRIDX01") &&
              integer(std::span(header).subspan(8)) == count,
          "invalid pack index header");
  blake3_hasher whole;
  blake3_hasher_init(&whole);
  blake3_hasher_update(&whole, header.data(), header.size());
  std::uint64_t offset = 8;
  std::optional<IndexKey> previous;
  Bytes buffer(page_bytes);
  for (std::size_t start = 0; start < count; start += records_per_page) {
    auto length = std::min(records_per_page, count - start) * record_size;
    auto data = std::span(buffer).first(length);
    read_exact(in, data);
    blake3_hasher_update(&whole, data.data(), data.size());
    for (std::size_t i = 0; i < length / record_size; ++i) {
      auto item = record(data, i);
      require(item.key.value[0] <= static_cast<std::uint8_t>(Kind::revision), "invalid packed object kind");
      require(!previous || *previous < item.key, "pack index must be strictly sorted");
      require(item.offset == offset && item.length >= 5 && item.length <= max_object_size + 1024 * 1024 &&
                  offset <= pack_bytes && item.length <= pack_bytes - offset,
              "invalid packed object range");
      offset += item.length;
      previous = item.key;
      result.add_filter(item.key);
    }
    result.pages_.push_back(Page{record(data, 0).key, *previous, digest(data)});
  }
  require(offset == pack_bytes, "unindexed pack data");
  Key sum{};
  blake3_hasher_finalize(&whole, sum.data(), sum.size());
  require(hex(sum) == expected_hash, "pack index hash mismatch");
  return result;
}
std::optional<std::size_t> PackIndex::candidate(const IndexKey &key) const {
  if (key < first() || key > last() || !may_contain(key))
    return {};
  auto after = std::upper_bound(pages_.begin(), pages_.end(), key,
                                [](const IndexKey &value, const Page &page) { return value < page.first; });
  auto page = static_cast<std::size_t>(after - pages_.begin() - 1);
  return key <= pages_[page].last ? std::optional(page) : std::nullopt;
}
std::unique_ptr<std::ifstream> PackIndex::open() const {
  check_file(path_, header_size + record_size * count_);
  auto result = std::make_unique<std::ifstream>(path_, std::ios::binary);
  require(result->is_open(), "cannot open pack index");
  return result;
}
Bytes PackIndex::read_page(std::size_t page, std::istream *stream) const {
  require(page < pages_.size(), "pack index page out of range");
  auto local = stream ? std::unique_ptr<std::ifstream>{} : open();
  auto &in = stream ? *stream : *local;
  in.clear();
  auto start = page * records_per_page;
  in.seekg(static_cast<std::streamoff>(header_size + start * record_size));
  Bytes data(std::min(records_per_page, count_ - start) * record_size);
  read_exact(in, data);
  require(digest(data) == pages_[page].digest, "pack index page changed since validation");
  return data;
}
} // namespace syrnck
