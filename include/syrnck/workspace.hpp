#pragma once
#include <syrnck/core.hpp>
namespace syrnck {
class WorkspaceObservation;
// Session-local manifest reuse for repeated captures. Each capture enumerates
// the complete selected tree; only files ingested by this session, still in its
// last published revision, and protected by unchanged strong local filesystem
// stamps can avoid source reads. Persistent advisory indexes are never trusted.
// Unsupported filesystems, recent/actively written files and invalidation use
// ordinary ingestion. Publication still verifies the complete object closure.
// Single caller; Store handles may be reopened between calls.
class WorkspaceCapture {
public:
  WorkspaceCapture(const fs::path &directory, const fs::path &store);
  ~WorkspaceCapture();
  WorkspaceCapture(WorkspaceCapture &&) noexcept;
  WorkspaceCapture &operator=(WorkspaceCapture &&) noexcept;
  WorkspaceCapture(const WorkspaceCapture &) = delete;
  WorkspaceCapture &operator=(const WorkspaceCapture &) = delete;
  // An optional bound observer receives the completely captured inventory
  // after publication, avoiding a redundant full source audit on the next tick.
  Json capture(const Store &, std::string_view branch, std::string_view message, IngestOptions,
               WorkspaceObservation * = nullptr);
  void invalidate();
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
// A hard reset captures tracked work, staged data and obstructing untracked
// regular files before replacing them. Unrelated untracked files stay in place.
// Application is fenced by the exact read-only plan and original branch head.
Json plan_native_hard_reset(const Store &, const fs::path &, std::string_view branch, std::string_view target,
                            bool verify_objects = false, OperationOptions = {});
Json native_hard_reset(const Store &, const fs::path &, std::string_view branch,
                       std::string_view expected_head, std::string_view target,
                       std::string_view expected_plan, OperationOptions = {});
// Null selects the complete tree; an explicit empty array selects no files.
// Changing a selection requires clean tracked work and an empty staging index.
// Logical history remains complete and HEAD is preserved during recovery.
Json plan_native_materialization(const Store &, const fs::path &,
    const std::optional<std::vector<std::string>> &prefixes, bool verify_objects = false,
    OperationOptions = {});
Json set_native_materialization(const Store &, const fs::path &,
    const std::optional<std::vector<std::string>> &prefixes, std::string_view expected_head,
    std::string_view expected_plan, OperationOptions = {});
// Archive the selected linked context and release its registration. Working
// files stay on disk; selected dirty work, complete history/indexes and all
// retained context references remain reconstructible through the archive.
Json plan_native_workspace_unregistration(const Store &, bool include_untracked = false,
    bool verify_objects = false, OperationOptions = {});
Json unregister_native_workspace(Store &, std::string_view expected_plan,
    bool include_untracked = false, OperationOptions = {});
Json native_workspace_archive(const Store &, std::string_view id,
    bool verify_objects = false, OperationOptions = {});
Json native_workspace_archives(const Store &, const std::optional<std::string> &after = {},
    std::size_t limit = 100, bool verify_objects = false, OperationOptions = {});
Json plan_native_workspace_archive_restore(const Store &, const fs::path &primary,
    std::string_view archive, std::string_view name, const fs::path &directory,
    NativeWorkspaceRestoreOptions = {}, bool verify_objects = false, OperationOptions = {});
Json restore_native_workspace_archive(const Store &, const fs::path &primary,
    std::string_view archive, std::string_view name, const fs::path &directory,
    std::string_view expected_plan, NativeWorkspaceRestoreOptions = {}, OperationOptions = {});
// Move an entire linked working directory within one filesystem, including
// ignored/untracked data. Run from primary; a pending move must be recovered
// before other mutations. Resume/abort never recursively delete a directory.
Json plan_native_workspace_relocation(const Store &, std::string_view name,
    const fs::path &destination, OperationOptions = {});
Json relocate_native_workspace(const Store &, std::string_view name,
    const fs::path &destination, std::string_view expected_plan, OperationOptions = {});
Json native_workspace_relocation_status(const Store &, OperationOptions = {});
Json recover_native_workspace_relocation(const Store &, bool abort = false, OperationOptions = {});
} // namespace syrnck
