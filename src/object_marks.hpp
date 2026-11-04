#pragma once
#include "pack_storage.hpp"

namespace syrnck {
// Operation-local reachability, tied to an immutable catalog snapshot. Packed
// keys use one bit each; only reachable keys absent from that catalog need IDs.
class ObjectMarks {
public:
  explicit ObjectMarks(const PackStorage &packs) : packs_(packs) {
    for (std::size_t i = 0; i < packs.pack_count(); ++i) {
      bits_.emplace_back((packs.entry_count(i) + 7) / 8, std::uint8_t{0});
      bytes_ += bits_.back().size();
    }
  }
  bool insert(const ObjectKey &key) {
    bool fresh;
    if (auto position = packs_.locate(key)) {
      auto &byte = bits_[position->segment][position->ordinal / 8];
      auto mask = static_cast<std::uint8_t>(1U << (position->ordinal % 8));
      fresh = (byte & mask) == 0;
      byte |= mask;
    } else
      fresh = loose_.insert(IndexKey::from(key)).second;
    if (fresh)
      ++count_;
    return fresh;
  }
  bool contains(PackStorage::Position position) const {
    return (bits_[position.segment][position.ordinal / 8] & (1U << (position.ordinal % 8))) != 0;
  }
  bool contains(const ObjectKey &key) const {
    auto position = packs_.locate(key);
    return position ? contains(*position) : loose_contains(key);
  }
  bool loose_contains(const ObjectKey &key) const { return loose_.contains(IndexKey::from(key)); }
  std::size_t size() const { return count_; }
  std::size_t packed_bytes() const { return bytes_; }
  std::size_t loose_size() const { return loose_.size(); }

private:
  const PackStorage &packs_;
  std::vector<Bytes> bits_;
  std::set<IndexKey> loose_;
  std::size_t count_ = 0, bytes_ = 0;
};
} // namespace syrnck
