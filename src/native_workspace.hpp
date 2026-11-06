#pragma once
#include "internal.hpp"
namespace syrnck {
// Decode an immutable completed receipt without old workspace dependencies or
// rereading the full reference table for each entry in a paginated inventory.
Json read_native_checkout_receipt(const Store &, const ObjectKey &, std::string_view id);

struct NativeFile {
  fs::path path;
  std::string relative;
  std::uint64_t size;
  fs::file_time_type modified;
  bool executable;
};
struct NativeContent {
  std::uint64_t size;
  std::string hash;
  bool operator==(const NativeContent &) const = default;
};
struct NativeSnapshotFile {
  std::string manifest;
  NativeContent content;
  bool executable;
  Json json() const {
    return Json{{"manifest", manifest.empty() ? Json{} : Json(manifest)},
                {"size", content.size},
                {"file_hash", content.hash},
                {"executable", executable}};
  }
};
using NativeSnapshot = std::map<std::string, NativeSnapshotFile>;
NativeSnapshot read_native_snapshot(const Store &, std::string_view, OperationOptions);
NativeContent hash_native_file(const NativeFile &, Bytes &buffer, const OperationOptions &);
bool native_link_like(const fs::path &, const fs::file_status &);
void verify_native_checkout_namespace(const Store &, const fs::path &, const std::vector<std::string> &target,
                                      const std::vector<std::string> &removed, OperationOptions,
                                      bool final = true, bool transitions = false);
// Shared snapshot selection for native commit and working-directory status.
void visit_native_files(const fs::path &directory, const fs::path &store,
                        const std::function<void(const NativeFile &)> &, OperationOptions);
// Enumerate only one file/subtree with the same selection rules as a full
// scan. Return the replacement scope: an absent, ignored or regular-file
// ancestor can broaden it. Caller replaces that complete prefix and validates
// the union with retained paths. Protected storage and case aliases refuse.
std::string visit_native_scope(const fs::path &directory, const fs::path &store, std::string_view path,
                                const std::function<void(const NativeFile &)> &, OperationOptions);
// Targeted inspection with shared ignore rules. Only enumerate ancestor
// directories; reject case aliases, links/reparse points and protected storage.
std::optional<NativeFile> inspect_native_file(const fs::path &directory, const fs::path &store,
                                              std::string_view path, OperationOptions);
Json ingest_native_file(const Store &, const NativeFile &, IngestOptions, std::string_view changed_message);
Json compare_native_snapshots(const Store &, const std::optional<std::string> &source,
                              const std::optional<std::string> &target, OperationOptions);
Json compare_native_file_maps(const NativeSnapshot &, const NativeSnapshot &, OperationOptions);
Json compare_native_workspace_maps(const NativeSnapshot &, const NativeSnapshot &, OperationOptions);
// Also capture the complete selected inventory, including unsupported or
// uncached entries. Output changes only after cache publication succeeds.
Json indexed_native_observation(const Store &, const fs::path &, const std::optional<std::string> &,
                                NativeSnapshot &, OperationOptions);
Json plan_native_checkout_files(const Store &, const fs::path &, std::string_view branch,
                                const NativeSnapshot &destination, OperationOptions);
} // namespace syrnck
