#pragma once
#include <syrnck/core.hpp>
namespace syrnck {
// A hard reset captures tracked work, staged data and obstructing untracked
// regular files before replacing them. Unrelated untracked files stay in place.
// Application is fenced by the exact read-only plan and original branch head.
Json plan_native_hard_reset(const Store &, const fs::path &, std::string_view branch, std::string_view target,
                            bool verify_objects = false, OperationOptions = {});
Json native_hard_reset(const Store &, const fs::path &, std::string_view branch,
                       std::string_view expected_head, std::string_view target,
                       std::string_view expected_plan, OperationOptions = {});
// Null selects the complete tree; an explicit empty array selects no files.
// Changing a selection requires clean tracked work and an empty staging index.
// Logical history remains complete and HEAD is preserved during recovery.
Json plan_native_materialization(const Store &, const fs::path &,
    const std::optional<std::vector<std::string>> &prefixes, bool verify_objects = false,
    OperationOptions = {});
Json set_native_materialization(const Store &, const fs::path &,
    const std::optional<std::vector<std::string>> &prefixes, std::string_view expected_head,
    std::string_view expected_plan, OperationOptions = {});
} // namespace syrnck
