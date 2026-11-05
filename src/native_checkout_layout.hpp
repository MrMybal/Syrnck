#pragma once
#include "native_workspace.hpp"
namespace syrnck {
// A namespace transition evacuates every changed source before installing any
// target. Only snapshot-derived directories may be removed, nonrecursively.
struct NativeCheckoutLayout {
  bool transition = false;
  std::set<std::string> source_directories, target_directories;
  std::set<std::string> remove_directories, rollback_directories, evacuated;
  NativeCheckoutLayout(const NativeSnapshot &, const NativeSnapshot &, OperationOptions);
};
} // namespace syrnck
