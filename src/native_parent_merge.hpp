#pragma once
#include "native_merge_base.hpp"
namespace syrnck {
struct NativeParentMerge {
  std::string id;
  NativeSnapshot files;
  std::set<std::string> ambiguous, reusable;
  std::map<std::string, std::string> provenance;
  Json report;
};
NativeParentMerge merge_native_parents(const Store &, const NativeAncestryGraph &,
                                       const std::vector<std::string> &parents, const NativeMergeBaseLimits &,
                                       const NativeSnapshotReader &, NativeTextMergeBatch *,
                                       NativeRenameBatch *, const std::function<void(std::size_t)> &,
                                       OperationOptions);
} // namespace syrnck
