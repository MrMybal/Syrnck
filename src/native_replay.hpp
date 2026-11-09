#pragma once
#include "native_workspace.hpp"
#include "native_text_merge.hpp"
namespace syrnck {
struct NativeReplayPlan {
  std::string head;
  NativeSnapshot files;
  Json report;
  NativeGeneratedText generated{};
};
NativeReplayPlan prepare_native_replay(const Store &, const fs::path &, std::string_view branch,
                                       std::string_view source, NativeReplayKind,
                                       std::optional<std::uint32_t> mainline, bool verify_objects,
                                       OperationOptions, const NativeResolutionSet * = nullptr,
                                       const TextDiffOptions * = nullptr,
                                       const NativeRenameOptions * = nullptr);
} // namespace syrnck
