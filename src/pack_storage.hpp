#pragma once
#include "internal.hpp"
#include "pack_index.hpp"
#include <fstream>
#include <list>

namespace syrnck {
// Immutable pack catalog. config.json is the only publication pointer.
class PackStorage {
public:
  class ReadSession {
  public:
    explicit ReadSession(const PackStorage &owner) : owner_(owner) {
      (void)owner_.metadata();
      ++owner_.read_depth_;
    }
    ~ReadSession() {
      if (--owner_.read_depth_ == 0)
        owner_.close_streams();
    }
    ReadSession(const ReadSession &) = delete;
    ReadSession &operator=(const ReadSession &) = delete;

  private:
    const PackStorage &owner_;
  };
  static constexpr std::size_t max_entries = 1024 * 1024;
  static constexpr std::size_t max_catalog_entries = 4 * max_entries, max_packs = 1024;
  static constexpr std::size_t max_open_streams = 8;
  struct IndexStats {
    std::uint64_t page_reads = 0, cache_hits = 0;
    std::size_t directory_bytes = 0, filter_bytes = 0, cache_bytes = 0, cached_pages = 0;
  };
  struct Position {
    std::size_t segment, ordinal;
    std::uint32_t length;
  };
  using KeyVisitor = std::function<void(const ObjectKey &)>;
  using KeySource = std::function<void(const KeyVisitor &)>;
  using KeyNext = std::function<std::optional<ObjectKey>()>;
  using KeySequence = std::function<KeyNext()>;
  explicit PackStorage(fs::path root, std::size_t index_cache_bytes = 4 * 1024 * 1024);
  void invalidate() {
    valid_ = false;
    close_streams();
  }
  void load(const Json &metadata);
  const Json &metadata() const;
  Json catalog() const;
  std::size_t pack_count() const;
  std::size_t open_streams() const;
  std::size_t open_index_streams() const;
  IndexStats index_stats() const;
  void set_index_cache_limit(std::size_t);
  void clear_index_cache() const;
  std::vector<std::vector<ObjectKey>> groups() const;
  std::size_t entry_count(std::size_t segment) const;
  // Each callback receives an independent key. Nested reads/lookups are safe,
  // even with the page cache disabled; publication during a visit is forbidden.
  void visit(std::size_t segment, const std::function<void(const ObjectKey &, Position)> &) const;
  void visit_kind(std::size_t segment, Kind, const std::function<void(const ObjectKey &, Position)> &) const;
  std::optional<Position> locate(const ObjectKey &) const;
  bool contains(const ObjectKey &) const;
  std::vector<ObjectKey> keys() const;
  // Ordered, single-pass merge with one owned page per active pack. The catalog
  // must outlive this cursor and remain unchanged until the cursor is destroyed.
  KeyNext ordered_keys() const;
  Bytes read(const ObjectKey &) const;
  std::uint64_t stored_size(const ObjectKey &) const;
  // Reuse one handle only within a serialized operation. No handle survives
  // between calls that can replace the pack path or publish a new generation.
  ReadSession read_session() const {
    return ReadSession(*this);
  }
  static PackStorage build(const fs::path &root, const std::vector<ObjectKey> &,
                           const std::function<Bytes(const ObjectKey &)> &verified_envelope,
                           std::uint64_t max_bytes = INT64_MAX,
                           std::size_t index_cache_bytes = 4 * 1024 * 1024);
  static PackStorage build_stream(const fs::path &, std::size_t count, const KeySource &,
                                  const std::function<Bytes(const ObjectKey &)> &verified_envelope,
                                  std::uint64_t max_bytes = INT64_MAX,
                                  std::size_t index_cache_bytes = 4 * 1024 * 1024, std::stop_token = {});
  static PackStorage build_many(const fs::path &, const std::vector<ObjectKey> &,
                                const std::function<Bytes(const ObjectKey &)> &,
                                const std::function<std::uint64_t(const ObjectKey &)> &size,
                                const PackOptions &, std::size_t available_packs = max_packs,
                                std::size_t index_cache_bytes = 4 * 1024 * 1024);
  static void validate_options(const PackOptions &);
  // The factory supplies two independent, identical ordered passes. Planning
  // retains only per-output-pack counts, byte sizes and sequence digests.
  static PackStorage build_many_stream(const fs::path &, std::size_t count, const KeySequence &,
                                       const std::function<Bytes(const ObjectKey &)> &,
                                       const std::function<std::uint64_t(const ObjectKey &)> &size,
                                       const PackOptions &, std::size_t available_packs = max_packs,
                                       std::size_t index_cache_bytes = 4 * 1024 * 1024, std::stop_token = {});
  // Returns physical files/bytes removed. Only generated, inactive names qualify.
  std::pair<std::size_t, std::uint64_t> cleanup(bool remove = true, std::stop_token = {}) const;

private:
  struct Entry {
    std::uint64_t offset;
    std::uint32_t length;
    std::size_t segment;
    std::size_t ordinal;
  };
  struct Segment {
    Json metadata;
    PackIndex index;
    mutable std::unique_ptr<std::ifstream> stream;
    mutable std::uint64_t touched = 0;
    mutable std::unique_ptr<std::ifstream> index_stream;
    mutable std::uint64_t index_touched = 0;
  };
  fs::path directory_;
  Json metadata_ = nullptr;
  std::vector<Segment> segments_;
  using PageKey = std::pair<std::size_t, std::size_t>;
  struct CachedPage {
    Bytes data;
    std::list<PageKey>::iterator lru;
  };
  std::size_t index_cache_limit_;
  mutable std::map<PageKey, CachedPage> index_cache_;
  mutable std::list<PageKey> index_lru_;
  mutable Bytes index_scratch_;
  mutable std::size_t index_cached_bytes_ = 0;
  mutable std::optional<std::pair<IndexKey, Entry>> last_lookup_;
  mutable std::uint64_t index_page_reads_ = 0, index_cache_hits_ = 0;
  bool valid_ = true;
  mutable std::size_t read_depth_ = 0;
  mutable std::uint64_t read_clock_ = 0;
  std::unique_ptr<std::ifstream> open(std::size_t segment) const;
  std::istream *index_stream(std::size_t segment) const;
  void close_streams() const;
  const Bytes &index_page(std::size_t segment, std::size_t page) const;
  std::optional<Entry> find(const ObjectKey &) const;
  void validate_unique() const;
  void visit_range(std::size_t segment, std::optional<Kind>,
                   const std::function<void(const ObjectKey &, Position)> &) const;
  void check_directory() const;
  fs::path path(std::size_t segment, std::string_view extension) const;
  void check_file(const fs::path &, std::uint64_t size) const;
};
} // namespace syrnck
