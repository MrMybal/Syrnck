#pragma once
#include "native_branch.hpp"
namespace syrnck {
inline constexpr std::string_view native_branch_archive_prefix = "archive/branches/";
struct NativeBranchArchive {
  std::string id, name;
  ObjectKey target{Kind::revision, {}};
  std::optional<ObjectKey> index;
  std::string prefix() const {
    return std::string(native_branch_archive_prefix) + id;
  }
  Json json() const {
    return Json{{"version", 1},
                {"type", "native-branch-archive"},
                {"name", name},
                {"revision", target.id},
                {"index", index ? Json(index->id) : Json{}}};
  }
  Refs roots() const {
    Refs result{{prefix() + "/record", {Kind::chunk, id}}, {prefix() + "/target", target}};
    if (index)
      result.emplace(prefix() + "/index", *index);
    return result;
  }
  // 0 is vacant, 1 is the exact complete archive, -1 is a conflicting root set.
  int root_state(const Refs &refs) const {
    const auto wanted = roots();
    const auto start = prefix() + '/';
    std::size_t found = 0;
    for (auto it = refs.lower_bound(start); it != refs.end() && it->first.starts_with(start); ++it) {
      const auto expected = wanted.find(it->first);
      if (expected == wanted.end() || expected->second != it->second)
        return -1;
      ++found;
    }
    return !found ? 0 : found == wanted.size() ? 1 : -1;
  }
  std::vector<RefUpdate> retention_updates(const Refs &refs) const {
    return {
        {prefix() + "/record", native_branch_lookup(refs, prefix() + "/record"), ObjectKey{Kind::chunk, id}},
        {prefix() + "/target", native_branch_lookup(refs, prefix() + "/target"), target},
        {prefix() + "/index", native_branch_lookup(refs, prefix() + "/index"), index}};
  }
  NativeBranchState state(const Store &store, OperationOptions options) const {
    Refs refs{{"heads/" + name, target}};
    if (index)
      refs.emplace(native_staging_reference(store, name), *index);
    return NativeBranchState(store, refs, name, options);
  }
};
NativeBranchArchive read_native_branch_archive(const Store &, const Refs &, std::string_view,
                                               OperationOptions);
} // namespace syrnck
