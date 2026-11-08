#pragma once
#include "native_workspace.hpp"
namespace syrnck {
struct NativeMaterializationPlan {
  std::string context, branch, head;
  std::optional<ObjectKey> selection, index;
  NativeMaterializationState source;
  NativeMaterialization target;
  Json report;
};
NativeMaterializationPlan prepare_native_materialization(const Store &, const fs::path &,
    const std::optional<std::vector<std::string>> &, bool, OperationOptions);
Json plan_native_materialization_files(const Store &, const fs::path &, std::string_view branch,
    std::string_view revision, const NativeMaterialization &source,
    const NativeMaterialization &target, OperationOptions);
bool validate_native_materialization_transition(const Store &, std::string_view context,
    const Refs &before, const Refs &after);
} // namespace syrnck
