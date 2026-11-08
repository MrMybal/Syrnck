#pragma once
#include <syrnck/native_net.hpp>
#include <syrnck/watcher.hpp>
namespace syrnck::net {
struct NativePushSyncOptions {
  std::string remote_branch = "main";
  bool capture_changes = true;
};
// Session-local observation and durable acknowledged history. Each tick uses
// the caller's currently locked Store; waiting between ticks needs no Store.
// This direction publishes native snapshots and never replaces working files.
class NativePushSync {
public:
  NativePushSync(const Store &, const fs::path &directory, NativePushSyncOptions = {});
  ~NativePushSync();
  NativePushSync(NativePushSync &&) noexcept;
  NativePushSync &operator=(NativePushSync &&) noexcept;
  NativePushSync(const NativePushSync &) = delete;
  NativePushSync &operator=(const NativePushSync &) = delete;
  Json tick(Store &, const Remote &, const WorkspaceChanges & = {}, OperationOptions = {}, TransferOptions = {});
  void invalidate();
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace syrnck::net
