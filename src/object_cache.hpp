#pragma once
#include <list>
#include <syrnck/core.hpp>

namespace syrnck {
// Process-local evidence, never loaded from disk. A lookup requires the digest
// of the envelope just read from disk, not a timestamp/size approximation.
class ObjectCache {
public:
  explicit ObjectCache(StoreOptions options) : options_(options) {}
  ~ObjectCache();
  ObjectCache(const ObjectCache &) = delete;
  ObjectCache &operator=(const ObjectCache &) = delete;
  struct Hit {
    bool verified = false;
    const Bytes *payload = nullptr;
  };
  Hit lookup(const ObjectKey &, const Key &digest);
  void remember(const ObjectKey &, const Key &digest, const Bytes &payload);
  void erase(const ObjectKey &);
  void clear() noexcept;
  std::size_t entries() const {
    return records_.size();
  }
  std::size_t payloads() const {
    return payload_order_.size();
  }
  std::size_t bytes() const {
    return bytes_;
  }
  bool enabled() const {
    return options_.verified_cache_entries != 0;
  }

private:
  using Order = std::list<ObjectKey>;
  struct Record {
    Key digest{};
    Bytes payload;
    Order::iterator position;
    std::optional<Order::iterator> payload_position;
  };
  void drop_payload(Record &) noexcept;
  StoreOptions options_;
  std::map<ObjectKey, Record> records_;
  Order order_, payload_order_;
  std::size_t bytes_ = 0;
};
} // namespace syrnck
