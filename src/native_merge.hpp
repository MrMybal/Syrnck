#pragma once
#include "native_workspace.hpp"
namespace syrnck {
struct NativeFileMergeResult {
  NativeSnapshot files;
  Json changes = Json::array(), conflicts = Json::array();
  Json resolved = Json::array();
  std::vector<ObjectKey> extra_roots;
  std::size_t processed = 0, paths = 0;
};
void require_native_resolution_context(const NativeResolutionSet &, std::string_view operation,
                                       std::string_view head, std::string_view source,
                                       const std::optional<std::string> &base,
                                       const std::optional<std::string> &parent = {});
NativeFileMergeResult merge_native_files(const Store &, const NativeSnapshot &base,
                                         const NativeSnapshot &current, const NativeSnapshot &incoming,
                                         OperationOptions, const NativeResolutionSet * = nullptr);
struct NativeMergePlan {
  std::string head;
  std::optional<std::string> base;
  NativeSnapshot files;
  Json report;
};
NativeMergePlan prepare_native_merge(const Store &, const fs::path &, std::string_view branch,
                                     std::string_view source, const NativeMergeOptions &, bool verify_objects,
                                     OperationOptions);
} // namespace syrnck
