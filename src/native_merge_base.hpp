#pragma once
#include "native_graph.hpp"
#include "native_merge.hpp"
namespace syrnck {
struct NativeMergeBase {
  std::optional<std::string> id;
  NativeSnapshot files;
  std::set<std::string> ambiguous;
  Json report;
};
NativeMergeBase synthesize_native_merge_base(const Store &, const NativeAncestryGraph &,
                                             std::vector<std::string> bases, const NativeMergeBaseLimits &,
                                             OperationOptions, NativeTextMergeBatch *, NativeRenameBatch *);
} // namespace syrnck
