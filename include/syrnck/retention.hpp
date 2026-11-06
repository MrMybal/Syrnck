#pragma once
#include <syrnck/core.hpp>
namespace syrnck {
// Retire simple native revision archives or immutable native-fetch roots only
// after a separate verified backup can reconstruct their complete closures.
// Objects are not deleted; GC remains a separate operation.
Json plan_archive_retirement(const Store &, const Store &backup, const std::vector<std::string> &references,
                             OperationOptions = {});
Json retire_archives(const Store &, const Store &backup, const std::vector<std::string> &references,
                     std::string_view expected_plan, OperationOptions = {});
} // namespace syrnck
