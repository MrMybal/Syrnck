#pragma once
#include "native_workspace.hpp"
namespace syrnck {
struct NativeFileMergeResult {
  NativeSnapshot files;
  Json changes = Json::array(), conflicts = Json::array();
  std::size_t processed = 0, paths = 0;
};
NativeFileMergeResult merge_native_files(const NativeSnapshot &base, const NativeSnapshot &current,
                                         const NativeSnapshot &incoming, OperationOptions);
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
