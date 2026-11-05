#pragma once
#include "native_merge.hpp"
namespace syrnck {
struct NativeRebasePlan {
  std::string head, target;
  NativeSnapshot files;
  Json report;
};
using NativeRebaseSink = std::function<void(const Json &, const std::string &)>;
NativeRebasePlan prepare_native_rebase(const Store &, const fs::path &, std::string_view branch,
                                       std::string_view base, std::string_view onto,
                                       const NativeRebaseOptions &, bool verify_objects, OperationOptions,
                                       const NativeRebaseSink & = {});
} // namespace syrnck
