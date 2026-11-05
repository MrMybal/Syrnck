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
struct RefUpdate {
  std::string name;
  std::optional<ObjectKey> expected, value;
};
// Local recovery metadata. Endpoint is a hash of the canonical URL, never a credential.
struct TransferSession {
  std::string id, direction, endpoint, reference, state = "transferring";
  ObjectKey root{Kind::manifest, {}};
  std::optional<ObjectKey> expected;
  std::optional<std::string> upload_lease;
  std::optional<std::string> read_lease;
  static std::string identity(std::string_view direction, std::string_view endpoint, const ObjectKey &,
                              std::string_view reference);
  void validate() const;
  Json json() const;
  static TransferSession parse(const Json &);
};
struct LeaseRejected : Error {
  using Error::Error;
};
struct UploadLease {
  std::string id, reference, fence, state = "active";
  ObjectKey root{Kind::manifest, {}};
  std::optional<ObjectKey> expected;
  std::uint64_t expires_at = 0;
  // Null keeps legacy global retention; a value enables paged selective retention.
  std::optional<std::uint32_t> inventory_pages;
  std::string inventory_hash;
  void validate() const;
  Json json() const;
  static UploadLease parse(const Json &);
};
struct ReadLease {
  std::string id, fence, state = "active";
  ObjectKey root{Kind::manifest, {}};
  std::uint64_t expires_at = 0;
  void validate() const;
  Json json() const;
  static ReadLease parse(const Json &);
};
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
  transferring,
  planning,
  packing
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
  // Partial reads verify the manifest and intersecting chunks. Only a whole-file
  // range also verifies file_hash; unrelated chunks need not be present.
  void reconstruct_range(std::string_view id, std::ostream &, std::uint64_t offset, std::uint64_t length,
                         OperationOptions = {}) const;
  void restore_file_range(std::string_view id, const fs::path &destination, std::uint64_t offset,
                          std::uint64_t length, OperationOptions = {}) const;
  // Metadata-only chunk reuse plan. Source offsets identify matching chunks;
  // callers must still verify payloads and the complete reconstructed file.
  Json compare_files(std::string_view source, std::string_view target, OperationOptions = {}) const;
  Refs refs() const;
  void set_ref(std::string_view, const ObjectKey &, std::stop_token = {}) const;
  bool compare_set_ref(std::string_view, const std::optional<ObjectKey> &, const ObjectKey &,
                       std::stop_token = {}) const;
  void remove_ref(std::string_view) const;
  // Compare all expected values, verify replacement closures, then publish
  // one refs document. Null value removes a ref; null expected means absent.
  bool compare_update_refs(const std::vector<RefUpdate> &, OperationOptions = {}) const;
  std::vector<TransferSession> transfer_sessions(std::stop_token = {}) const;
  std::optional<TransferSession> transfer_session(std::string_view id) const;
  void write_transfer_session(const TransferSession &, std::stop_token = {}) const;
  void remove_transfer_session(std::string_view id) const;
  // Preview by default. Only complete receipts are removable; payloads and refs are untouched.
  Json prune_transfer_sessions(bool remove = false, OperationOptions = {}) const;
  std::vector<UploadLease> upload_leases(std::stop_token = {}) const;
  std::optional<UploadLease> upload_lease(std::string_view id) const;
  UploadLease acquire_upload_lease(std::string_view id, const ObjectKey &, std::string_view reference,
                                   const std::optional<ObjectKey> &expected, std::uint64_t seconds,
                                   const std::optional<std::string> &fence = {},
                                   bool inventory = false) const;
  void register_upload_inventory(std::string_view id, std::string_view fence, std::uint32_t page,
                                 const std::vector<ObjectKey> &) const;
  bool publish_upload_lease(std::string_view id, std::string_view fence) const;
  void confirm_upload_lease(std::string_view id, std::string_view fence) const;
  void release_upload_lease(std::string_view id) const;
  std::size_t prune_upload_leases() const;
  std::vector<ReadLease> read_leases(std::stop_token = {}) const;
  std::optional<ReadLease> read_lease(std::string_view id) const;
  ReadLease acquire_read_lease(std::string_view id, const ObjectKey &, std::uint64_t seconds,
                               const std::optional<std::string> &fence = {}) const;
  void release_read_lease(std::string_view id, const std::optional<std::string> &fence = {}) const;
  std::size_t prune_read_leases() const;
  std::set<ObjectKey> closure(const std::vector<ObjectKey> &, bool verify_files = false, std::stop_token = {},
                              ProgressCallback = {}) const;
  std::vector<ObjectKey> objects() const;
  std::size_t fsck(OperationOptions = {}) const;
  Json gc(bool remove = false, OperationOptions = {}) const;
  // Physical v2 for a single pack, v3 for multiple packs; logical objects stay v1.
  // Validates all objects before publication and removal of loose copies.
  Json pack() const;
  Json pack(PackOptions, OperationOptions = {}) const;
  std::string commit_directory(const fs::path &, std::string_view branch, std::string_view message) const;
  std::string commit_directory(const fs::path &, std::string_view branch, std::string_view message,
                               IngestOptions) const;
  // Read-only comparison with a native snapshot, or an empty baseline.
  // Files are hashed in full; timestamps never establish content equality.
  Json status_directory(const fs::path &, const std::optional<std::string> &revision = {},
                        OperationOptions = {}) const;
  // Current file entries only: no parent traversal or chunk payload reads.
  Json compare_revisions(std::string_view source, std::string_view target, OperationOptions = {}) const;
  // Branch-local immutable staging snapshot. Paths are repository-relative
  // files/directory prefixes, or "." for the entire selected workspace.
  std::string stage_directory(const fs::path &, std::string_view branch,
                              const std::vector<std::string> &paths) const;
  std::string stage_directory(const fs::path &, std::string_view branch,
                              const std::vector<std::string> &paths, IngestOptions) const;
  Json staged_status(std::string_view branch, OperationOptions = {}) const;
  // Read-only branch -> staging -> working-directory comparisons.
  Json workspace_status(const fs::path &, std::string_view branch, OperationOptions = {}) const;
  // Conservative read-only preflight, including ignored/untracked obstructions.
  // This reports proposed actions and blockers; it never changes working files.
  Json plan_checkout(const fs::path &, std::string_view branch, std::string_view target,
                     bool verify_objects = false, OperationOptions = {}) const;
  // Apply the clean-workspace policy, preserving the previous head in an
  // archive. Interrupted operations require explicit recovery; no force mode.
  Json checkout_directory(const fs::path &, std::string_view branch, std::string_view target,
                          OperationOptions = {}) const;
  Json checkout_status(const std::optional<std::string> &transaction = {}) const;
  Json recover_checkout(bool abort = false, OperationOptions = {}) const;
  // Symbolic current branch; legacy stores implicitly select main. Switching
  // journals workspace changes and publishes HEAD without moving either branch.
  Json native_head() const;
  Json plan_switch(const fs::path &, std::string_view branch, bool verify_objects = false,
                   OperationOptions = {}) const;
  Json switch_directory(const fs::path &, std::string_view branch, OperationOptions = {}) const;
  // Native release labels are immutable through this API. Deletion archives
  // their target and optional annotation atomically; archives remain retained.
  Json native_tags(const std::optional<std::string> &after = {}, std::size_t limit = 100,
                   OperationOptions = {}) const;
  Json native_tag(std::string_view name, bool verify_objects = false, OperationOptions = {}) const;
  Json create_native_tag(std::string_view name, std::string_view revision,
                         const std::optional<std::string> &message = {}, OperationOptions = {}) const;
  Json delete_native_tag(std::string_view name, std::string_view expected_revision,
                         OperationOptions = {}) const;
  Json native_tag_archive(std::string_view id, bool verify_objects = false, OperationOptions = {}) const;
  Json restore_native_tag(std::string_view archive_id, OperationOptions = {}) const;
  // Exact ID, HEAD, heads/NAME, tags/NAME, or an unambiguous bare branch/tag.
  // Resolves a revision's metadata only; complete closure verification is separate.
  std::string resolve_native_revision(std::string_view selector, OperationOptions = {}) const;
  // No paths clears the index, including a stale index. Selected paths restore
  // their recorded baseline entries. Working files are never changed.
  std::optional<std::string> reset_stage(std::string_view branch, const std::vector<std::string> &paths = {},
                                         OperationOptions = {}) const;
  std::string commit_staged(std::string_view branch, std::string_view message, OperationOptions = {}) const;
  // Replace exactly the expected tip, preserving its parents and archiving it
  // under archive/amend/ID in the same reference transaction. Directory/message
  // modes reject an existing index; staged mode consumes a fresh index atomically.
  std::string amend_directory(const fs::path &, std::string_view branch, std::string_view expected_revision,
                              std::string_view message) const;
  std::string amend_directory(const fs::path &, std::string_view branch, std::string_view expected_revision,
                              std::string_view message, IngestOptions) const;
  std::string amend_staged(std::string_view branch, std::string_view expected_revision,
                           std::string_view message, OperationOptions = {}) const;
  std::string amend_message(std::string_view branch, std::string_view expected_revision,
                            std::string_view message, OperationOptions = {}) const;
  std::string detach_history(std::string_view branch, std::string_view message) const;
  void export_revision(std::string_view id, const fs::path &destination, OperationOptions = {}) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  fs::path object_path(const ObjectKey &) const;
  Bytes read_envelope(const ObjectKey &) const;
  Bytes decode_envelope(const ObjectKey &, std::span<const std::uint8_t>) const;
  Bytes load_object(const ObjectKey &, bool presence_only) const;
  void write_refs(const Refs &, std::stop_token = {}) const;
  void transfer_gc_roots(std::vector<ObjectKey> &complete, std::vector<ObjectKey> &partial,
                         std::stop_token) const;
  std::vector<UploadLease> prepare_upload_gc(std::stop_token, bool remove) const;
  void visit_upload_inventory(const UploadLease &, const std::function<void(const ObjectKey &)> &,
                              std::stop_token) const;
  void read_gc_roots(std::vector<ObjectKey> &, std::stop_token, bool remove) const;
};
} // namespace syrnck
