#pragma once
#include "internal.hpp"
#include <fstream>

namespace syrnck {
struct IndexKey {
  std::array<std::uint8_t, 33> value{};
  auto operator<=>(const IndexKey &) const = default;
  static IndexKey from(const ObjectKey &);
  ObjectKey object() const;
};
struct IndexRecord {
  IndexKey key;
  std::uint64_t offset;
  std::uint32_t length;
};
// Validated snapshot of an immutable SYRIDX01 file. Only fences, page digests
// and a negative-key filter stay resident; records are read in bounded pages.
class PackIndex {
public:
  static constexpr std::size_t record_size = 45, header_size = 16, records_per_page = 256;
  static constexpr std::size_t page_bytes = record_size * records_per_page;
  static PackIndex scan(const fs::path &, std::size_t count, std::uint64_t pack_bytes,
                        std::string_view expected_hash);
  std::optional<std::size_t> candidate(const IndexKey &) const;
  // Half-open page range intersecting this kind, including boundary pages.
  std::pair<std::size_t, std::size_t> kind_pages(Kind) const;
  std::unique_ptr<std::ifstream> open() const;
  Bytes read_page(std::size_t, std::istream *stream = nullptr) const;
  std::size_t page_count() const {
    return pages_.size();
  }
  std::size_t count() const {
    return count_;
  }
  std::size_t directory_bytes() const {
    return pages_.capacity() * sizeof(Page);
  }
  std::size_t filter_bytes() const {
    return filter_.capacity();
  }
  const IndexKey &first() const {
    return pages_.front().first;
  }
  const IndexKey &last() const {
    return pages_.back().last;
  }
  static IndexRecord record(std::span<const std::uint8_t>, std::size_t ordinal);

private:
  struct Page {
    IndexKey first, last;
    Key digest;
  };
  static_assert(sizeof(Page) == 98);
  fs::path path_;
  std::size_t count_ = 0;
  std::vector<Page> pages_;
  Bytes filter_;
  void add_filter(const IndexKey &);
  bool may_contain(const IndexKey &) const;
};
} // namespace syrnck
