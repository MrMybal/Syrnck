#pragma once
#include <syrnck/core.hpp>
#include <chrono>
namespace syrnck {
struct WorkspaceWatchOptions {
  bool polling = false;
  std::size_t max_paths = 4096;
  std::size_t max_directories = 65536;
};
struct WorkspaceChanges {
  std::vector<std::string> paths;
  bool full_scan = false, overflow = false, polling = false;
  bool changed() const { return full_scan || polling || !paths.empty(); }
};
// Change hints, never permission to replace files. Arm before the initial full
// observation; a lost journal requires a full observation before using hints.
// Single caller; stop tokens may be requested from another thread.
class WorkspaceWatcher {
public:
  WorkspaceWatcher(const fs::path &directory, const fs::path &store, WorkspaceWatchOptions = {},
                    std::stop_token = {});
  ~WorkspaceWatcher();
  WorkspaceWatcher(WorkspaceWatcher &&) noexcept;
  WorkspaceWatcher &operator=(WorkspaceWatcher &&) noexcept;
  WorkspaceWatcher(const WorkspaceWatcher &) = delete;
  WorkspaceWatcher &operator=(const WorkspaceWatcher &) = delete;
  std::string_view backend() const;
  WorkspaceChanges wait(std::chrono::milliseconds timeout, std::stop_token = {});
  // Caller lost its observation state; the next wait returns immediately with
  // full_scan=true. This does not discard pending filesystem notifications.
  void invalidate();
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
// Session-local complete observation. Feed every native hint batch from a
// watcher armed before the first status. Lost continuity, restart or switching
// workspaces requires invalidate()/full_scan; persisted hash caches alone do
// not establish an inventory. Advisory only: mutations do their own preflight.
class WorkspaceObservation {
public:
  WorkspaceObservation(const fs::path &directory, const fs::path &store);
  ~WorkspaceObservation();
  WorkspaceObservation(WorkspaceObservation &&) noexcept;
  WorkspaceObservation &operator=(WorkspaceObservation &&) noexcept;
  WorkspaceObservation(const WorkspaceObservation &) = delete;
  WorkspaceObservation &operator=(const WorkspaceObservation &) = delete;
  Json status(const Store &, const std::optional<std::string> &revision,
              const WorkspaceChanges &, OperationOptions = {});
  Json workspace_status(const Store &, std::string_view branch, const WorkspaceChanges &,
                        OperationOptions = {});
  void invalidate();
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace syrnck
