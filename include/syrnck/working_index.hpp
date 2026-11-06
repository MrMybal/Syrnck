#pragma once
#include <syrnck/core.hpp>
namespace syrnck {
// Opt-in, local advisory cache. Filesystem identities/change times supplement
// size/mtime. Unsupported filesystems and recent writes are hashed in full.
// Enumeration/ignore evaluation still run; mutation preflights never use this.
Json indexed_native_status(const Store &, const fs::path &,
                           const std::optional<std::string> &revision = {}, OperationOptions = {});
Json indexed_native_workspace_status(const Store &, const fs::path &, std::string_view branch,
                                     OperationOptions = {});
// Only remove the local pointer. Cached objects are ordinary unrooted chunks
// and can be collected by GC; no working files, staging or refs are changed.
Json clear_native_working_index(const Store &, OperationOptions = {});
} // namespace syrnck
