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
struct NativeBlameOptions {
  TextDiffOptions text{};
  std::size_t max_revisions = 1000;
  // Aggregate decoded revision/manifest metadata and reconstructed file bytes.
  // This is an input budget, not a bound on the process's complete memory use.
  std::size_t max_input_bytes = 64 * 1024 * 1024;
  bool follow_renames = true;
  bool first_parent = false;
};
// Verified UTF-8 line attribution over ordered merge parents. Exact,
// unambiguous renames can be followed; copies/edited renames are not guessed.
// All lines retain their exact terminators. Binary or limited reports contain
// no partial attribution. Does not read or modify the working directory.
Json native_blame(const Store &, std::string_view revision, std::string_view path,
                   NativeBlameOptions = {}, OperationOptions = {});
} // namespace syrnck
