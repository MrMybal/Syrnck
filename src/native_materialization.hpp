#pragma once
#include "internal.hpp"
namespace syrnck {
// Physical projection only. Revisions and frozen indexes always keep full trees.
// A missing policy means all paths; an explicit empty prefix array means none.
class NativeMaterialization {
  bool complete_ = true;
  std::vector<std::string> prefixes_;
  std::set<std::string> folded_;
  bool covers(std::string path) const;

public:
  static constexpr std::size_t max_prefixes = 1024, max_prefix_bytes = 64 * 1024;
  NativeMaterialization() = default;
  static NativeMaterialization from_prefixes(std::vector<std::string>);
  static NativeMaterialization from_definition(const Json &);
  static NativeMaterialization decode(std::span<const std::uint8_t>);
  bool complete() const { return complete_; }
  const std::vector<std::string> &prefixes() const { return prefixes_; }
  bool includes(std::string_view path) const;
  // True for a covered subtree or an ancestor needed to reach selected paths.
  // The workspace root is always traversable, even with an empty projection.
  bool intersects(std::string_view path) const;
  Json definition() const;
  std::string encode() const;
  std::string identity() const;
};
struct NativeMaterializationState {
  NativeMaterialization policy;
  std::optional<ObjectKey> root;
  std::string reference;
};
bool native_materialization_reference(std::string_view, std::string *context = nullptr);
NativeMaterializationState read_native_materialization(const Store &);
NativeMaterializationState read_native_materialization(const Store &, std::string_view context,
                                                        const Refs &, bool require_feature = true);
void validate_native_materialization_requirements(const Store &, const Refs &);
bool validate_native_materialization_changes(const Store &, const Refs &, const Refs &);
void require_complete_native_materialization(const Store &);
void export_native_materialization(const Store &, std::string_view revision, const fs::path &,
                                   const NativeMaterialization &, OperationOptions);
template<class Map> void project_native_materialization(Map &files, const NativeMaterialization &policy) {
  if (policy.complete()) return;
  for (auto entry = files.begin(); entry != files.end();)
    if (policy.includes(entry->first)) ++entry;
    else entry = files.erase(entry);
}
} // namespace syrnck
