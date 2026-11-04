#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <istream>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <ostream>
#include <set>
#include <span>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <vector>

namespace syrnck {
namespace fs = std::filesystem;
using Json = nlohmann::ordered_json;
using Bytes = std::vector<std::uint8_t>;
using Key = std::array<std::uint8_t, 32>;
inline constexpr std::size_t max_object_size = 16 * 1024 * 1024;
struct Error : std::runtime_error {
  using std::runtime_error::runtime_error;
};
struct MissingObject : Error {
  using Error::Error;
};
struct Cancelled : Error {
  using Error::Error;
};
void require(bool condition, std::string_view message);
enum class Kind { chunk, manifest, revision };
std::string_view kind_name(Kind kind);
Kind parse_kind(std::string_view text);
void validate_id(std::string_view id);
void validate_ref(std::string_view name);
void validate_path(std::string_view path);
std::string hash(std::span<const std::uint8_t> data);
std::string object_id(Kind kind, std::span<const std::uint8_t> data);
std::string hex(std::span<const std::uint8_t> data);
Bytes unhex(std::string_view text);
Bytes random_bytes(std::size_t size);
std::string utf8(const fs::path &path);
fs::path path_from_utf8(std::string_view path);
struct ObjectKey {
  Kind kind;
  std::string id;
  bool operator==(const ObjectKey &) const = default;
  auto operator<=>(const ObjectKey &) const = default;
};
Json key_json(const ObjectKey &key);
ObjectKey parse_key(const Json &value);
using Refs = std::map<std::string, ObjectKey>;
struct Chunking {
  bool fixed = false;
  std::uint32_t min = 256 * 1024, avg = 1024 * 1024, max = 4 * 1024 * 1024;
  static Chunking fixed_size(std::uint32_t size);
  void validate() const;
  Json json() const;
};
struct IngestStats {
  std::uint64_t file_bytes = 0, new_bytes = 0, reused_bytes = 0;
  std::size_t blocks = 0;
  // Raw bytes represented by queued/running/completed jobs awaiting publication.
  // This is not total process memory (codecs, read buffer and metadata are extra).
  std::size_t peak_in_flight_bytes = 0;
  std::size_t peak_in_flight_blocks = 0;
  Json json() const;
};
enum class ProgressPhase {
  scanning,
  ingesting,
  reconstructing,
  publishing,
  verifying,
  exporting,
  transferring
};
struct ProgressEvent {
  ProgressPhase phase = ProgressPhase::ingesting;
  // Borrowed context, valid only during the callback. Never reenter the Store.
  std::string_view path{};
  std::uint64_t bytes = 0;
  std::optional<std::uint64_t> total_bytes{};
  bool complete = false;
  std::size_t buffered_blocks = 0;
};
using ProgressCallback = std::function<void(const ProgressEvent &)>;
struct OperationOptions {
  std::stop_token stop{};
  ProgressCallback progress{};
};
struct IngestOptions {
  // One worker keeps the sequential path; 2..64 enables the bounded pipeline.
  // At most 2 * workers distinct blocks may await publication, independently
  // of the byte budget (also bounds task metadata for tiny fixed blocks).
  std::size_t workers = 1;
  std::size_t max_in_flight_bytes = 32 * 1024 * 1024;
  // Cooperative cancellation between reads, blocks and publications.
  // A native read/codec call already in progress is allowed to finish.
  std::stop_token stop{};
  // Synchronous coordinator-thread observer; exceptions stop the operation.
  ProgressCallback progress{};
};
struct StoreOptions {
  std::size_t decoded_cache_bytes = 64 * 1024 * 1024;
  std::size_t verified_cache_entries = 8192;
  std::size_t pack_index_cache_bytes = 4 * 1024 * 1024;
};
struct PackOptions {
  // Hard limits for newly built packs, including their 8-byte header.
  std::uint64_t max_pack_bytes = 512ULL * 1024 * 1024;
  std::size_t max_objects_per_pack = 65536;
  // Default maintenance appends only new loose objects. Compact rewrites all.
  bool compact = false;
};
// Local preferences, separate from physical config.json and logical objects.
// Secrets and destructive actions are never persisted here.
struct RepositorySettings {
  std::size_t ingest_workers = 1;
  std::size_t ingest_buffer_bytes = 32 * 1024 * 1024;
  std::uint64_t pack_max_bytes = 512ULL * 1024 * 1024;
  std::size_t pack_max_objects = 65536;
  void validate() const;
  Json json() const;
  static RepositorySettings parse(const Json &);
  IngestOptions ingest_options() const;
  PackOptions pack_options() const;
};
struct StoreIoStats {
  std::uint64_t object_reads = 0, envelope_bytes_read = 0, decoded_objects = 0;
  std::uint64_t cache_presence_hits = 0, cache_data_hits = 0;
  std::size_t cached_objects = 0, cached_payloads = 0, cached_bytes = 0;
  // Snapshot of index directory/filter/page payload allocations; excludes
  // allocator/container overhead, temporary buffers and bulk object inventories.
  std::size_t index_directory_bytes = 0, index_filter_bytes = 0;
  std::size_t index_cached_bytes = 0, index_cached_pages = 0;
  Json json() const;
};
std::vector<ObjectKey> dependencies(Kind kind, std::span<const std::uint8_t> bytes);
void validate_object(Kind kind, std::span<const std::uint8_t> bytes);

/// Exclusive store handle. Serialize access across threads; processes fail fast when busy.
class Store {
public:
  Store(const fs::path &root, std::optional<Key> key = {}, bool create = false, StoreOptions = {});
  ~Store();
  Store(Store &&) noexcept;
  Store &operator=(Store &&) noexcept;
  Store(const Store &) = delete;
  Store &operator=(const Store &) = delete;
  const fs::path &root() const;
  bool encrypted() const;
  RepositorySettings settings() const;
  void set_settings(const RepositorySettings &) const;
  StoreIoStats io_stats() const;
  void reset_io_stats() const;
  void clear_cache() const;
  bool contains(const ObjectKey &) const;
  Bytes get(const ObjectKey &) const;
  std::pair<ObjectKey, bool> put(Kind, std::span<const std::uint8_t>) const;
  bool import(const ObjectKey &, std::span<const std::uint8_t>) const;
  Json metadata(Kind, std::string_view id) const;
  std::pair<std::string, IngestStats> ingest(std::istream &, Chunking = {}) const;
  std::pair<std::string, IngestStats> ingest(std::istream &, Chunking, IngestOptions) const;
  void reconstruct(std::string_view id, std::ostream &, OperationOptions = {}) const;
  void restore_file(std::string_view id, const fs::path &destination, OperationOptions = {}) const;
  Refs refs() const;
  void set_ref(std::string_view, const ObjectKey &, std::stop_token = {}) const;
  bool compare_set_ref(std::string_view, const std::optional<ObjectKey> &, const ObjectKey &,
                       std::stop_token = {}) const;
  void remove_ref(std::string_view) const;
  std::set<ObjectKey> closure(const std::vector<ObjectKey> &, bool verify_files = false, std::stop_token = {},
                              ProgressCallback = {}) const;
  std::vector<ObjectKey> objects() const;
  std::size_t fsck(OperationOptions = {}) const;
  Json gc(bool remove = false) const;
  // Physical v2 for a single pack, v3 for multiple packs; logical objects stay v1.
  // Validates all objects before publication and removal of loose copies.
  Json pack() const;
  Json pack(PackOptions) const;
  std::string commit_directory(const fs::path &, std::string_view branch, std::string_view message) const;
  std::string commit_directory(const fs::path &, std::string_view branch, std::string_view message,
                               IngestOptions) const;
  std::string detach_history(std::string_view branch, std::string_view message) const;
  void export_revision(std::string_view id, const fs::path &destination) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  fs::path object_path(const ObjectKey &) const;
  Bytes read_envelope(const ObjectKey &) const;
  Bytes decode_envelope(const ObjectKey &, std::span<const std::uint8_t>) const;
  Bytes load_object(const ObjectKey &, bool presence_only) const;
  void write_refs(const Refs &, std::stop_token = {}) const;
};
} // namespace syrnck
