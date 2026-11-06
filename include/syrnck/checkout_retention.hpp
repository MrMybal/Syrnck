#pragma once
#include <syrnck/core.hpp>
namespace syrnck {
// Historical checkout receipts and their independently retained target roots.
// Backup/restored Stores can keep targets without local workspace receipts.
Json native_checkout_receipts(const Store &, const std::optional<std::string> &after = {},
                              std::size_t limit = 100, bool verify_objects = false, OperationOptions = {});
// Remove selected completed receipt/target pairs only after verified external
// backup coverage. Optional source-archive retirement is part of the same CAS.
// Active/private checkout state is never removed and GC remains explicit.
Json plan_checkout_retirement(const Store &, const Store &backup, const std::vector<std::string> &ids,
                              bool include_source_archives = false, OperationOptions = {});
Json retire_checkouts(const Store &, const Store &backup, const std::vector<std::string> &ids,
                      std::string_view expected_plan, bool include_source_archives = false,
                      OperationOptions = {});
} // namespace syrnck
