#pragma once
#include "native_workspace.hpp"
namespace syrnck {
struct NativeReplayPlan {
  std::string head;
  NativeSnapshot files;
  Json report;
};
NativeReplayPlan prepare_native_replay(const Store &, const fs::path &, std::string_view branch,
                                       std::string_view source, NativeReplayKind,
                                       std::optional<std::uint32_t> mainline, bool verify_objects,
                                       OperationOptions);
} // namespace syrnck
