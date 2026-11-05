#pragma once
#include "native_merge.hpp"
#include "native_rewrite.hpp"
namespace syrnck {
struct NativeStageRebasePlan {
  std::string target;
  Json candidate, report;
};
NativeStageRebasePlan prepare_native_stage_rebase(const Store &, const NativeRewriteState &,
                                                  bool verify_objects, OperationOptions);
} // namespace syrnck
