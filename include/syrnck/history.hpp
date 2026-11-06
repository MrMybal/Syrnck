#pragma once
#include <syrnck/core.hpp>
namespace syrnck {
// Read-only exact-content rename detection. Ambiguous duplicates remain
// additions/removals; unchanged copies are not presented as renames.
Json native_diff_renames(const Store &, const std::optional<std::string> &source,
                         const std::optional<std::string> &target, OperationOptions = {});
struct NativeFileHistoryOptions {
  std::size_t limit = 100;
  std::size_t max_revisions = 1000;
  bool follow_renames = true;
};
// First-parent file lineage. next_revision/next_path form an explicit cursor.
Json native_file_history(const Store &, std::string_view revision, std::string_view path,
                         NativeFileHistoryOptions = {}, OperationOptions = {});
} // namespace syrnck
