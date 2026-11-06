#pragma once
#include "internal.hpp"
#include <syrnck/i18n.hpp>
namespace syrnck::cli {
struct NativeWatchCommand {
  std::optional<std::string> branch, revision;
  bool combined = false, polling = false;
  std::uint64_t max_paths = 4096;
  std::uint64_t interval_ms = 500, debounce_ms = 75, audit_ms = 300000, duration_ms = 0, max_updates = 0;
};
int watch_native(const fs::path &directory, const fs::path &store, const NativeWatchCommand &,
                 const std::function<std::unique_ptr<Store>()> &open_store, Language, OperationOptions);
} // namespace syrnck::cli
