#pragma once
#include "native_merge.hpp"
#include <syrnck/stash.hpp>
namespace syrnck {
struct NativeStashEntry {
  std::string id, branch, base, work, message;
  std::optional<std::string> index;
  std::uint64_t generation = 0, created_at = 0;
  ObjectKey record{Kind::chunk, {}};
  NativeMaterialization materialization;
  Json json() const;
  std::string name() const {
    return "stash/" + id + "/record";
  }
  std::string work_name() const {
    return "stash/" + id + "/work";
  }
  std::string archive_name() const {
    return "stash-archives/" + id + "/record";
  }
  std::string archive_work() const {
    return "archive/stash/" + work;
  }
};
NativeStashEntry read_native_stash(const Store &, std::string_view id, bool archived = false,
                                   OperationOptions = {});
std::vector<RefUpdate> native_stash_guard(const Store &, const NativeStashEntry &, bool remove);
struct NativeStashApplyPlan {
  NativeStashEntry saved;
  std::string head, target;
  std::optional<std::string> index;
  Json working_revision, index_revision, report;
  NativeSnapshot files;
  NativeGeneratedText generated;
  std::optional<ObjectKey> selection;
  NativeMaterializationState materialization;
};
NativeStashApplyPlan prepare_native_stash_apply(const Store &, const fs::path &, std::string_view branch,
                                                std::string_view id, const NativeStashApplyOptions &,
                                                bool verify_objects, OperationOptions);
Json checkout_native_stash(const Store &, const fs::path &, std::string_view branch,
                           const NativeStashApplyPlan &, bool pop, OperationOptions);
Json native_stash_working_parents(std::string_view head, std::string_view work,
                                  const std::optional<std::string> &index);
} // namespace syrnck
