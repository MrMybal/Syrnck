#pragma once
#include "native_workspace.hpp"
#include <syrnck/workspace.hpp>
namespace syrnck {
struct NativeHardResetPlan {
  std::string head;
  std::optional<ObjectKey> index;
  NativeSnapshot files;
  std::vector<NativeFile> working;
  std::set<std::string> directories;
  Json report;
  std::optional<ObjectKey> selection{};
};
NativeHardResetPlan prepare_native_hard_reset(const Store &, const fs::path &, std::string_view branch,
                                              std::string_view target, bool verify_objects, OperationOptions);
Json plan_native_checkout_source(const Store &, const fs::path &, std::string_view branch,
                                 std::string_view source, std::string_view target,
                                 const std::set<std::string> &directories, OperationOptions);
} // namespace syrnck
