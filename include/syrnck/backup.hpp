#pragma once
#include <syrnck/core.hpp>
namespace syrnck {
// Offline logical snapshots of retained objects. Both Stores remain exclusively
// locked by their owners; the destination must be a new path. Workspace binding
// and completed checkout receipts are omitted, not transplanted to another host.
Json create_backup(const Store &, const fs::path &destination, std::optional<Key> destination_key = {},
                   OperationOptions = {});
Json verify_backup(const Store &, OperationOptions = {});
Json restore_backup(const Store &, const fs::path &destination, std::optional<Key> destination_key = {},
                    OperationOptions = {});
// Missing objects only. Dry run by default; any corrupt existing object blocks
// the entire repair before writes. References, packs and unknown bytes survive.
Json repair_from_backup(const Store &destination, const Store &backup, bool apply = false,
                        OperationOptions = {});
// Rebuild portable current roots in a new Store, using a verified backup for
// missing/corrupt objects. Neither input's files, packs nor refs are replaced.
// Current objects absent from an older backup are preserved when still valid.
Json plan_repaired_copy(const Store &source, const Store &backup, OperationOptions = {});
Json create_repaired_copy(const Store &source, const Store &backup, const fs::path &destination,
                          std::optional<Key> destination_key = {}, OperationOptions = {});
} // namespace syrnck
