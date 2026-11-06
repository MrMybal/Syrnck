#pragma once
#include "internal.hpp"
namespace syrnck {
inline constexpr std::size_t watch_journal_limit = 2 * 1024 * 1024;
void validate_watch_journal(const Json &, OperationOptions = {});
// A distinct session lock; holding it never holds the Store lock. Check the
// filesystem entry before FileLock opens it, including Windows reparse points.
std::unique_ptr<FileLock> lock_watch_journal(const fs::path &storage);
} // namespace syrnck
