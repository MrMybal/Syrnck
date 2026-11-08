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
// Read-only line diffs. Inputs and JSON output have separate hard limits;
// an exceeded budget returns status=limited with no partial hunks.
struct TextDiffOptions {
  std::size_t context = 3;
  std::size_t max_file_bytes = 1024 * 1024;
  std::size_t max_lines = 50000;
  std::size_t max_work = 16 * 1024 * 1024;
  std::size_t max_output_bytes = 4 * 1024 * 1024;
  void validate() const;
};
Json compare_text(std::span<const std::uint8_t> source, std::span<const std::uint8_t> target,
                  TextDiffOptions = {}, OperationOptions = {});
// Conservative three-way line merge. Only status=clean includes content;
// conflicts, binary inputs and exceeded budgets never expose a partial result.
// Both edit searches share max_work; max_output_bytes bounds the complete JSON.
Json merge_text(std::span<const std::uint8_t> base, std::span<const std::uint8_t> current,
                std::span<const std::uint8_t> incoming, TextDiffOptions = {}, OperationOptions = {});
struct TextPatchResult {
  std::string content;
  std::size_t hunks = 0, additions = 0, deletions = 0;
};
// Exact single-file unified patch, with matching labels and no offset/fuzz.
// max_output_bytes bounds patch input; other text limits bound source/result/work.
TextPatchResult apply_text_patch(std::span<const std::uint8_t> source, std::span<const std::uint8_t> patch,
                                 std::string_view path, TextDiffOptions = {}, OperationOptions = {});
struct NativeGraphLimits {
  std::size_t max_revisions = 100000;
  std::size_t max_parent_links = 1000000;
};
enum class NativeReplayKind { cherry_pick, revert };
enum class NativeResetMode { soft, mixed };
enum class NativeResolutionChoice { current, incoming, base, remove, file };
struct NativeFileResolution {
  NativeResolutionChoice choice = NativeResolutionChoice::current;
  std::string manifest{};
  bool executable = false;
};
struct NativeResolutionSet {
  std::string operation, head, source;
  std::optional<std::string> base{};
  std::optional<std::string> parent{};
  std::map<std::string, NativeFileResolution> files;
  void validate() const;
  Json json() const;
  static NativeResolutionSet parse(const Json &);
  // Bounded, duplicate-key rejecting decoder for editable JSON documents.
  static NativeResolutionSet decode(std::span<const std::uint8_t>);
};
struct NativeMergeOptions {
  std::optional<std::string> base{};
  bool allow_unrelated_histories = false;
  NativeGraphLimits limits{};
  bool no_fast_forward = false;
  bool fast_forward_only = false;
  std::optional<NativeResolutionSet> resolutions{};
  // Opt in to bounded automatic text composition of unresolved content conflicts.
  // The shared native batch also caps attempts at 64 and inputs/results at 16 MiB.
  std::optional<TextDiffOptions> text_merge{};
};
struct NativeRebaseResolutionSet {
  std::string head, base, onto;
  std::map<std::string, NativeResolutionSet> steps;
  void validate() const;
  Json json() const;
  static NativeRebaseResolutionSet parse(const Json &);
  static NativeRebaseResolutionSet decode(std::span<const std::uint8_t>);
};
// Unified replay settings; legacy mainline/resolved entry points remain valid.
struct NativeReplayOptions {
  std::optional<std::uint32_t> mainline{};
  std::optional<NativeResolutionSet> resolutions{};
  std::optional<TextDiffOptions> text_merge{};
};
struct NativeRebaseTodoStep {
  std::string source, action = "pick";
  std::optional<std::string> message{};
};
struct NativeRebaseTodo {
  std::string head, base, onto;
  std::vector<NativeRebaseTodoStep> steps;
  void validate() const;
  Json json() const;
  static NativeRebaseTodo parse(const Json &);
  static NativeRebaseTodo decode(std::span<const std::uint8_t>);
};
struct NativeRebaseOptions {
  std::size_t max_commits = 1000;
  std::optional<NativeRebaseResolutionSet> resolutions{};
  // One shared text budget covers the entire range, including virtual results.
  std::optional<TextDiffOptions> text_merge{};
  // Explicit complete sequence: pick/drop/reword/squash/fixup, with reordering.
  std::optional<NativeRebaseTodo> todo{};
};
struct NativeStageRebaseOptions {
  std::optional<NativeResolutionSet> resolutions{};
  std::optional<TextDiffOptions> text_merge{};
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
struct NativeWorkspaceRestoreOptions {
  std::optional<std::string> branch{};
  bool detached = false;
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

/**
 * @doc api-store
 * @locale en
 * @title C++ Store API
 * @section API
 * @symbol syrnck::Store
 * Use `#include <syrnck/core.hpp>` and link `Syrnck::core`.
 *
 * ## Ownership and concurrency {#ownership}
 * `Store(root, key, create, options)` owns an exclusive repository lock. Keep the handle alive
 * for the complete operation and serialize all calls, including destruction. It is movable,
 * not copyable. Internal ingest workers do not make the handle safe for concurrent callers.
 *
 * ## Objects and retention {#objects}
 * `ingest(input, chunking)` returns a manifest ID and statistics; it does not publish a reference.
 * `get` verifies an object's typed identity. `reconstruct` verifies chunks and the complete file.
 * `set_ref` validates its complete closure before retention. `compare_update_refs` publishes
 * a group only when every expected value still matches; false means the caller must refresh.
 * See [storage](doc:storage), [ranges](doc:file-ranges), and [reference tables](doc:reference-tables).
 *
 * ## Errors and cancellation {#errors}
 * Catch `syrnck::Error`, with `MissingObject` and `Cancelled` as specialized errors.
 * OperationOptions supplies a stop token and synchronous progress observer; do not reenter
 * the Store from callbacks. Follow each operation's publication boundary and retention rules.
 * See [progress and cancellation](doc:operations) and [repository settings](doc:settings).
 */
/**
 * @doc api-store
 * @locale fr
 * @title API C++ du Store
 * @section API
 * @symbol syrnck::Store
 * Inclure `#include <syrnck/core.hpp>` et lier `Syrnck::core`.
 *
 * ## Propriété et concurrence {#ownership}
 * `Store(root, key, create, options)` détient le verrou exclusif du dépôt. Garder le handle
 * pendant toute l'opération et sérialiser les appels, destruction comprise. Il se déplace
 * mais ne se copie pas. Les workers internes ne rendent pas le handle utilisable en concurrence.
 *
 * ## Objets et rétention {#objects}
 * `ingest(input, chunking)` rend un ID de manifeste et des statistiques, sans publier de référence.
 * `get` vérifie l'identité typée ; `reconstruct` vérifie les blocs et le hash complet du fichier.
 * `set_ref` contrôle la fermeture complète avant rétention. `compare_update_refs` publie un groupe
 * seulement si toutes les valeurs attendues correspondent ; false impose de refaire la lecture.
 * Voir le [stockage](doc:storage), les [plages](doc:file-ranges) et les [références](doc:reference-tables).
 *
 * ## Erreurs et annulation {#errors}
 * Intercepter `syrnck::Error`, dont dérivent `MissingObject` et `Cancelled`.
 * OperationOptions fournit un jeton d'arrêt et un observateur synchrone ; ne pas réentrer
 * dans le Store depuis un callback. Respecter les frontières de publication et de rétention
 * propres à chaque opération. Voir la [progression](doc:operations) et les [préférences](doc:settings).
 */
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
  // Required capabilities persist across maintenance and copies. Older readers
  // reject the extended configuration before opening objects or workspace state.
  std::vector<std::string> required_features() const;
  void require_features(const std::vector<std::string> &, OperationOptions = {}) const;
  RepositorySettings settings() const;
  void set_settings(const RepositorySettings &) const;
  StoreIoStats io_stats() const;
  void reset_io_stats() const;
  void clear_cache() const;
  // Local observation checkpoint, independent of retained object roots. Null
  // when absent; compressed and encrypted with this Store's envelope key.
  Json workspace_watch_journal(OperationOptions = {}) const;
  void write_workspace_watch_journal(const Json &, OperationOptions = {}) const;
  Json clear_workspace_watch_journal(OperationOptions = {}) const;
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
  bool publish_upload_lease(std::string_view id, std::string_view fence, OperationOptions = {}) const;
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
  // One exact path, with verified complete file bytes. A null revision is an
  // empty snapshot. Oversized files are reported before any chunk is read.
  Json diff_revision_file(const std::optional<std::string> &source, const std::optional<std::string> &target,
                          std::string_view path, TextDiffOptions = {}, OperationOptions = {}) const;
  // Read-only content merge at one existing path in three explicit snapshots.
  // Only requested payloads are verified; this does not infer graph ancestry.
  Json merge_revision_file(std::string_view base, std::string_view current, std::string_view incoming,
                           std::string_view path, TextDiffOptions = {}, OperationOptions = {}) const;
  // Targeted working-file comparison. Null source means an empty snapshot;
  // ignored/obstructed paths fail rather than appearing as deletions.
  Json diff_working_file(const fs::path &directory, const std::optional<std::string> &source,
                         std::string_view path, TextDiffOptions = {}, OperationOptions = {}) const;
  // Use the frozen index when present, otherwise the branch head. Stale
  // indexes stay inspectable; report their original base/current head.
  Json diff_unstaged_file(const fs::path &directory, std::string_view branch, std::string_view path,
                          TextDiffOptions = {}, OperationOptions = {}) const;
  // Branch-local immutable staging snapshot. Paths are repository-relative
  // files/directory prefixes, or "." for the entire selected workspace.
  std::string stage_directory(const fs::path &, std::string_view branch,
                              const std::vector<std::string> &paths) const;
  std::string stage_directory(const fs::path &, std::string_view branch,
                              const std::vector<std::string> &paths, IngestOptions) const;
  Json staged_status(std::string_view branch, OperationOptions = {}) const;
  // Numbered text hunks against the frozen index/head. Applying an exact plan
  // retains the previous index and changes only selected text, never working files.
  Json plan_stage_hunks(const fs::path &, std::string_view branch, std::string_view path,
                        TextDiffOptions = {}, OperationOptions = {}) const;
  Json stage_hunks(const fs::path &, std::string_view branch, std::string_view path,
                   std::string_view expected_plan, const std::vector<std::size_t> &hunks,
                   TextDiffOptions = {}, OperationOptions = {}) const;
  // Remove selected baseline-to-index hunks without reading working files.
  // Previous preparations remain rooted in archive/stage-index/<old index>.
  Json plan_reset_hunks(std::string_view branch, std::string_view path, TextDiffOptions = {},
                        OperationOptions = {}) const;
  Json reset_hunks(std::string_view branch, std::string_view path, std::string_view expected_plan,
                   const std::vector<std::size_t> &hunks, TextDiffOptions = {}, OperationOptions = {}) const;
  // Individual add/remove rows have global change_number IDs within an exact
  // line plan. Refuses selections that join unterminated lines; no normalization.
  Json plan_stage_lines(const fs::path &, std::string_view branch, std::string_view path,
                        TextDiffOptions = {}, OperationOptions = {}) const;
  Json stage_lines(const fs::path &, std::string_view branch, std::string_view path,
                   std::string_view expected_plan, const std::vector<std::size_t> &lines,
                   TextDiffOptions = {}, OperationOptions = {}) const;
  Json plan_reset_lines(std::string_view branch, std::string_view path, TextDiffOptions = {},
                        OperationOptions = {}) const;
  Json reset_lines(std::string_view branch, std::string_view path, std::string_view expected_plan,
                   const std::vector<std::size_t> &lines, TextDiffOptions = {}, OperationOptions = {}) const;
  // Exact external patch against the frozen index/head, without working reads.
  // Publication retains the complete former index under archive/stage-index/ID.
  Json plan_stage_patch(std::string_view branch, std::string_view path, std::span<const std::uint8_t> patch,
                        TextDiffOptions = {}, OperationOptions = {}) const;
  Json stage_patch(std::string_view branch, std::string_view path, std::span<const std::uint8_t> patch,
                   std::string_view expected_plan, TextDiffOptions = {}, OperationOptions = {}) const;
  // Compare a frozen file against the index's original baseline, including
  // stale/unborn indexes. Does not read the working directory.
  Json diff_staged_file(std::string_view branch, std::string_view path, TextDiffOptions = {},
                        OperationOptions = {}) const;
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
  // Current symbolic branch or detached revision; legacy stores select main.
  // Switching journals workspace changes without moving named branch tips.
  Json native_head() const;
  // Opaque selector accepted by native workspace/staging/history operations.
  // A detached selector is not a branch name and cannot be published as one.
  std::string native_target() const;
  // Select a registered workspace for this handle. Empty selects the primary
  // legacy workspace; this never changes another handle's selection.
  void use_native_workspace(std::string_view name);
  std::string_view native_workspace_name() const;
  std::string native_workspace_reference(std::string_view local) const;
  void validate_native_workspace(const fs::path &) const;
  Json register_native_workspace(const fs::path &primary, std::string_view name,
                                 const fs::path &directory, OperationOptions = {}) const;
  Json restore_native_workspace(const fs::path &primary, std::string_view name, const fs::path &directory,
                                 NativeWorkspaceRestoreOptions = {}, OperationOptions = {}) const;
  Json native_workspaces(OperationOptions = {}) const;
  // Bounded pages of existing branch heads and their frozen staging state.
  Json native_branches(const std::optional<std::string> &after = {}, std::size_t limit = 100,
                       bool verify_objects = false, OperationOptions = {}) const;
  Json create_native_branch(std::string_view name, std::string_view revision, OperationOptions = {}) const;
  Json plan_switch(const fs::path &, std::string_view branch, bool verify_objects = false,
                   OperationOptions = {}) const;
  Json switch_directory(const fs::path &, std::string_view branch, OperationOptions = {}) const;
  Json plan_detached_checkout(const fs::path &, std::string_view revision, bool verify_objects = false,
                              OperationOptions = {}) const;
  Json detach_checkout(const fs::path &, std::string_view revision, OperationOptions = {}) const;
  // Branch deletion preserves its tip and optional index in a retained archive.
  // Selected branches and active checkouts cannot be deleted. A supplied index
  // ID both authorizes archiving it and guards that exact frozen snapshot.
  Json plan_delete_native_branch(std::string_view name, bool archive_index = false,
                                 bool verify_objects = false, OperationOptions = {}) const;
  Json delete_native_branch(std::string_view name, std::string_view expected_revision,
                            const std::optional<std::string> &expected_index = {},
                            OperationOptions = {}) const;
  Json native_branch_archives(const std::optional<std::string> &after = {}, std::size_t limit = 100,
                              bool verify_objects = false, OperationOptions = {}) const;
  Json native_branch_archive(std::string_view archive_id, bool verify_objects = false,
                             OperationOptions = {}) const;
  // Restore archived heads/indexes only into absent destination slots. A new
  // name rewrites just the staging marker, preserving frozen entries and base.
  Json plan_restore_native_branch(std::string_view archive_id, const std::optional<std::string> &name = {},
                                  bool verify_objects = false, OperationOptions = {}) const;
  Json restore_native_branch(std::string_view archive_id, const std::optional<std::string> &name = {},
                             OperationOptions = {}) const;
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
  Json native_tag_archives(const std::optional<std::string> &after = {}, std::size_t limit = 100,
                           bool verify_objects = false, OperationOptions = {}) const;
  Json restore_native_tag(std::string_view archive_id, OperationOptions = {}) const;
  // Exact ID, HEAD, heads/NAME, tags/NAME, or an unambiguous bare branch/tag.
  // Resolves a revision's metadata only; complete closure verification is separate.
  std::string resolve_native_revision(std::string_view selector, OperationOptions = {}) const;
  // Complete, bounded metadata-only traversal. Reports every best common
  // ancestor, including multiple bases in a criss-cross history.
  Json native_ancestry(std::string_view source, std::string_view target, NativeGraphLimits = {},
                       OperationOptions = {}) const;
  // Read-only topology, file-level three-way merge and clean-workspace plan.
  // Multiple best bases require an explicit selection; unrelated roots opt in.
  Json plan_merge(const fs::path &, std::string_view branch, std::string_view source,
                  const NativeMergeOptions & = {}, bool verify_objects = false, OperationOptions = {}) const;
  // Verified no-op, fast-forward or two-parent merge through the checkout journal.
  Json merge_directory(const fs::path &, std::string_view branch, std::string_view expected_revision,
                       std::string_view source, std::string_view message, const NativeMergeOptions & = {},
                       OperationOptions = {}) const;
  // Replay the linear range (base, head] onto an exact new base. Planning never
  // stores candidates; virtual IDs preserve source messages and timestamps.
  Json plan_rebase(const fs::path &, std::string_view branch, std::string_view base, std::string_view onto,
                   const NativeRebaseOptions & = {}, bool verify_objects = false,
                   OperationOptions = {}) const;
  Json rebase_directory(const fs::path &, std::string_view branch, std::string_view expected_revision,
                        std::string_view base, std::string_view onto, const NativeRebaseOptions & = {},
                        OperationOptions = {}) const;
  // Read-only file-level three-way replay plus the existing clean-workspace
  // checkout preflight. Merge revisions require an explicit 1-based mainline.
  Json plan_replay(const fs::path &, std::string_view branch, std::string_view source, NativeReplayKind,
                   std::optional<std::uint32_t> mainline = {}, bool verify_objects = false,
                   OperationOptions = {}) const;
  Json plan_replay_resolved(const fs::path &, std::string_view branch, std::string_view source,
                            NativeReplayKind, const NativeResolutionSet &,
                            std::optional<std::uint32_t> mainline = {}, bool verify_objects = false,
                            OperationOptions = {}) const;
  Json plan_replay_with_options(const fs::path &, std::string_view branch, std::string_view source,
                                NativeReplayKind, const NativeReplayOptions &, bool verify_objects = false,
                                OperationOptions = {}) const;
  // Verify both inputs, create a child of the expected head and apply it through
  // the recoverable checkout journal. Conflicts/empty results never publish.
  Json replay_directory(const fs::path &, std::string_view branch, std::string_view expected_revision,
                        std::string_view source, NativeReplayKind, std::string_view message,
                        std::optional<std::uint32_t> mainline = {}, OperationOptions = {}) const;
  Json replay_directory_resolved(const fs::path &, std::string_view branch,
                                 std::string_view expected_revision, std::string_view source,
                                 NativeReplayKind, std::string_view message, const NativeResolutionSet &,
                                 std::optional<std::uint32_t> mainline = {}, OperationOptions = {}) const;
  Json replay_directory_with_options(const fs::path &, std::string_view branch,
                                     std::string_view expected_revision, std::string_view source,
                                     NativeReplayKind, std::string_view message, const NativeReplayOptions &,
                                     OperationOptions = {}) const;
  // No paths clears the index, including a stale index. Selected paths restore
  // their recorded baseline entries. Working files are never changed.
  std::optional<std::string> reset_stage(std::string_view branch, const std::vector<std::string> &paths = {},
                                         OperationOptions = {}) const;
  // Replay frozen staging changes onto the current branch head without reading
  // or changing working files. A ready plan contains a deterministic index ID.
  Json plan_rebase_stage(std::string_view branch, bool verify_objects = false, OperationOptions = {}) const;
  Json plan_rebase_stage_resolved(std::string_view branch, const NativeResolutionSet &,
                                  bool verify_objects = false, OperationOptions = {}) const;
  Json plan_rebase_stage_with_options(std::string_view branch, const NativeStageRebaseOptions &,
                                      bool verify_objects = false, OperationOptions = {}) const;
  // Verify retained bytes, archive the old index and replace it in one expected
  // branch/index reference transaction. The branch and working files do not move.
  Json rebase_stage(std::string_view branch, std::string_view expected_revision,
                    std::string_view expected_index, OperationOptions = {}) const;
  Json rebase_stage_resolved(std::string_view branch, std::string_view expected_revision,
                             std::string_view expected_index, const NativeResolutionSet &,
                             OperationOptions = {}) const;
  Json rebase_stage_with_options(std::string_view branch, std::string_view expected_revision,
                                 std::string_view expected_index, const NativeStageRebaseOptions &,
                                 OperationOptions = {}) const;
  // Branch resets preserve working files. Soft preserves prepared entries on
  // the new baseline; mixed archives and clears the index. Old roots publish
  // atomically with the expected branch/index transition.
  Json plan_reset_branch(std::string_view branch, std::string_view target, NativeResetMode,
                         bool verify_objects = false, OperationOptions = {}) const;
  Json reset_branch(std::string_view branch, std::string_view expected_revision, std::string_view target,
                    NativeResetMode, OperationOptions = {}) const;
  std::string commit_staged(std::string_view branch, std::string_view message, OperationOptions = {}) const;
  // Collapse 2..100000 linear tip commits, preserving the exact tip snapshot
  // and archiving former history. Merge commits in the range and staging block
  // publication. Working files are not accessed.
  Json plan_squash(std::string_view branch, std::size_t count, bool verify_objects = false,
                   OperationOptions = {}) const;
  Json squash_commits(std::string_view branch, std::string_view expected_revision, std::size_t count,
                      std::string_view message, OperationOptions = {}) const;
  // Read-only pages of owned revision archives, with full reference cursors.
  // Kinds: reset, reset-index, rebase-index, squash, amend, checkout and detached.
  Json native_archives(const std::optional<std::string> &kind = {},
                       const std::optional<std::string> &after = {}, std::size_t limit = 100,
                       bool verify_objects = false, OperationOptions = {}) const;
  Json native_archive(std::string_view reference, bool verify_objects = false, OperationOptions = {}) const;
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
