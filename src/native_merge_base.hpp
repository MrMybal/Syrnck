#pragma once
#include "native_graph.hpp"
#include "native_merge.hpp"
namespace syrnck {
using NativeSnapshotReader = std::function<NativeSnapshot(std::string_view, OperationOptions)>;
struct NativeMergeBaseCallbacks {
  NativeSnapshotReader read{};
  std::function<void(std::size_t)> work{};
};
struct NativeMergeBase {
  std::optional<std::string> id;
  NativeSnapshot files;
  std::set<std::string> ambiguous;
  Json report;
};
NativeMergeBase synthesize_native_merge_base(const Store &, const NativeAncestryGraph &,
                                             std::vector<std::string> bases, const NativeMergeBaseLimits &,
                                             OperationOptions, NativeTextMergeBatch *, NativeRenameBatch *,
                                             NativeMergeBaseCallbacks = {});
} // namespace syrnck
