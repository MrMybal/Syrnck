#include "object_cache.hpp"
#include <monocypher.h>

namespace syrnck {
ObjectCache::~ObjectCache() {
  clear();
}
void ObjectCache::drop_payload(Record &record) noexcept {
  if (record.payload_position) {
    payload_order_.erase(*record.payload_position);
    record.payload_position.reset();
  }
  bytes_ -= record.payload.size();
  if (!record.payload.empty())
    crypto_wipe(record.payload.data(), record.payload.size());
  Bytes{}.swap(record.payload);
}
void ObjectCache::erase(const ObjectKey &key) {
  auto found = records_.find(key);
  if (found == records_.end())
    return;
  drop_payload(found->second);
  order_.erase(found->second.position);
  records_.erase(found);
}
void ObjectCache::clear() noexcept {
  for (auto &[key, record] : records_) {
    (void)key;
    if (!record.payload.empty())
      crypto_wipe(record.payload.data(), record.payload.size());
  }
  records_.clear();
  order_.clear();
  payload_order_.clear();
  bytes_ = 0;
}
ObjectCache::Hit ObjectCache::lookup(const ObjectKey &key, const Key &digest) {
  auto found = records_.find(key);
  if (found == records_.end())
    return {};
  if (found->second.digest != digest) {
    erase(key);
    return {};
  }
  auto &record = found->second;
  order_.splice(order_.begin(), order_, record.position);
  if (record.payload_position) {
    payload_order_.splice(payload_order_.begin(), payload_order_, *record.payload_position);
    return {true, &record.payload};
  }
  return {true, nullptr};
}
void ObjectCache::remember(const ObjectKey &key, const Key &digest, const Bytes &payload) {
  if (!enabled())
    return;
  try {
    auto found = records_.find(key);
    if (found != records_.end() && found->second.digest != digest) {
      erase(key);
      found = records_.end();
    }
    if (found == records_.end()) {
      while (records_.size() >= options_.verified_cache_entries)
        erase(order_.back());
      order_.push_front(key);
      found = records_.emplace(key, Record{digest, {}, order_.begin(), {}}).first;
    } else
      order_.splice(order_.begin(), order_, found->second.position);
    auto &record = found->second;
    if (record.payload_position || payload.size() > options_.decoded_cache_bytes)
      return;
    while (payload.size() > options_.decoded_cache_bytes - bytes_)
      drop_payload(records_.at(payload_order_.back()));
    record.payload = payload;
    bytes_ += payload.size();
    payload_order_.push_front(key);
    record.payload_position = payload_order_.begin();
  } catch (const std::bad_alloc &) {
    // This optional accelerator must not turn a verified read into a failure.
    // Clear also tolerates partially inserted list/map nodes after allocation failure.
    clear();
  }
}
} // namespace syrnck
