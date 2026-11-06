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
} // namespace syrnck
