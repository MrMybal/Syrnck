#pragma once
#include <syrnck/native_net.hpp>
#include <syrnck/watcher.hpp>
namespace syrnck::net {
// Logical revision transport for the resident workspace state machine. Session
// identifiers belong to the transport's physical journal; roots are original
// revisions. Implementations must record the publication observer synchronously
// before completing that journal, including reconciliation after a lost reply.
struct NativeSyncSession {
  std::string id;
  ObjectKey root{Kind::revision, {}};
  std::string state;
};
struct NativeSyncObservation {
  std::optional<ObjectKey> root;
  std::optional<std::string> blocked_reason;
};
class NativeSyncRemote {
public:
  virtual ~NativeSyncRemote() = default;
  virtual std::string endpoint_fingerprint() const = 0;
  virtual std::vector<NativeSyncSession> sessions(const Store &, std::string_view branch,
                                                  bool include_completed, OperationOptions,
                                                  TransferOptions) = 0;
  virtual NativeSyncObservation observe(const Store &, std::string_view branch, OperationOptions,
                                        TransferOptions) = 0;
  virtual Json fetch(const Store &, std::string_view branch, std::string_view tracking_name, OperationOptions,
                     TransferOptions) = 0;
  virtual Json resume(const Store &, std::string_view session, OperationOptions, TransferOptions,
                      const PublicationObserver &) = 0;
  virtual bool conflicted(const Store &, const ObjectKey &, std::string_view branch) const = 0;
  virtual Json push_expected(const Store &, const ObjectKey &, std::string_view branch,
                             const std::optional<ObjectKey> &expected, OperationOptions, TransferOptions,
                             const PublicationObserver &) = 0;
};
struct NativePushSyncOptions {
  std::string remote_branch = "main";
  bool capture_changes = true;
  bool receive_changes = false;
  bool merge_changes = false;
  bool conflict_copies = false;
  std::optional<NativeRenameOptions> renames{};
};
// Session-local observation and durable acknowledged history. Each tick uses
// the caller's currently locked Store; waiting between ticks needs no Store.
// The default direction only publishes. Explicit reception uses the existing
// clean-workspace checkout transaction and preserves both sides of merges.
class NativePushSync {
public:
  NativePushSync(const Store &, const fs::path &directory, NativePushSyncOptions = {});
  ~NativePushSync();
  NativePushSync(NativePushSync &&) noexcept;
  NativePushSync &operator=(NativePushSync &&) noexcept;
  NativePushSync(const NativePushSync &) = delete;
  NativePushSync &operator=(const NativePushSync &) = delete;
  Json tick(Store &, const Remote &, const WorkspaceChanges & = {}, OperationOptions = {},
            TransferOptions = {});
  Json tick(Store &, NativeSyncRemote &, const WorkspaceChanges & = {}, OperationOptions = {},
            TransferOptions = {});
  void invalidate();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
// General session names; the original publication-only spelling remains valid.
using NativeSyncOptions = NativePushSyncOptions;
using NativeSync = NativePushSync;
} // namespace syrnck::net
