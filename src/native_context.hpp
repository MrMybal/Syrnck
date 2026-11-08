#pragma once
#include "internal.hpp"
namespace syrnck {
struct NativeWorkspaceContext {
  std::string name;
  fs::path directory;
  ObjectKey registration{Kind::chunk, {}};
};
NativeWorkspaceContext read_native_workspace_context(const Store &, std::string_view name);
// Logical linked state only; excluded primary machine bindings are not read.
Json native_linked_workspace_rows(const Store &, const Refs &, OperationOptions);
// Check all workspace reservations before publishing shared branch changes.
void validate_native_branch_reservations(const Store &, const Refs &before, const Refs &after);
std::optional<std::string> native_branch_workspace_owner(const Store &, std::string_view branch);
void require_native_branch_available(const Store &, std::string_view branch);
// Context-aware filesystem location for disposable caches/session journals.
fs::path native_workspace_local_path(const Store &, std::string_view leaf);
fs::path native_context_local_path(const fs::path &storage, std::string_view context, std::string_view leaf);
} // namespace syrnck
