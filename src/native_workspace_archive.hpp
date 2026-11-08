#pragma once
#include "native_workspace.hpp"
#include "native_context.hpp"
namespace syrnck {
inline constexpr std::string_view native_workspace_archive_prefix = "archive/workspaces/";
struct NativeWorkspaceArchive {
  std::string id, name, nonce, head, work;
  std::optional<std::string> branch, index;
  NativeMaterialization materialization;
  Refs references;
  std::uint64_t created_at = 0;
  bool include_untracked = false;
  std::string prefix() const { return std::string(native_workspace_archive_prefix) + id; }
  Json json() const;
  Refs roots() const;
};
NativeWorkspaceArchive read_native_workspace_archive(const Store &, const Refs &,
    std::string_view id, OperationOptions = {});
void verify_native_workspace_archive(const Store &, const NativeWorkspaceArchive &, OperationOptions);
} // namespace syrnck
