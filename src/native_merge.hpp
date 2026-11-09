#pragma once
#include "native_workspace.hpp"
#include "native_text_merge.hpp"
#include "native_renames.hpp"
namespace syrnck {
struct NativeFileMergeResult {
  NativeSnapshot files;
  Json changes = Json::array(), conflicts = Json::array();
  Json resolved = Json::array();
  std::vector<ObjectKey> extra_roots;
  NativeGeneratedText generated{};
  Json text_merge;
  Json renames;
  std::size_t processed = 0, paths = 0;
};
struct NativeConflictCopyContext { std::string current, incoming; };
void require_native_resolution_context(const NativeResolutionSet &, std::string_view operation,
                                       std::string_view head, std::string_view source,
                                       const std::optional<std::string> &base,
                                       const std::optional<std::string> &parent = {});
NativeFileMergeResult merge_native_files(const Store &, const NativeSnapshot &base,
                                         const NativeSnapshot &current, const NativeSnapshot &incoming,
                                         OperationOptions, const NativeResolutionSet * = nullptr,
                                         const TextDiffOptions * = nullptr, NativeTextMergeBatch * = nullptr,
                                         const NativeConflictCopyContext * = nullptr,
                                         NativeRenameBatch * = nullptr, bool preserve_incoming_representation = false);
struct NativeMergePlan {
  std::string head;
  std::optional<std::string> base;
  NativeSnapshot files;
  Json report;
  NativeGeneratedText generated{};
};
NativeMergePlan prepare_native_merge(const Store &, const fs::path &, std::string_view branch,
                                     std::string_view source, const NativeMergeOptions &, bool verify_objects,
                                     OperationOptions);
} // namespace syrnck
