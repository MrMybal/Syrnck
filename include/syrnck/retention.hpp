#pragma once
#include <syrnck/core.hpp>
namespace syrnck {
// Select simple revision/native-fetch roots or complete archive/branches/ID and
// archive/tags/ID groups. Groups require the exact validated roots in the backup,
// including descriptor/index/annotation. Member-by-member retirement is refused.
// A separate verified backup must reconstruct every complete selected closure.
// Objects are not deleted; GC remains a separate operation.
Json plan_archive_retirement(const Store &, const Store &backup, const std::vector<std::string> &references,
                             OperationOptions = {});
Json retire_archives(const Store &, const Store &backup, const std::vector<std::string> &references,
                     std::string_view expected_plan, OperationOptions = {});
} // namespace syrnck
