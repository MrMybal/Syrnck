#pragma once
#include "native_context.hpp"
#include "native_directory_identity.hpp"
namespace syrnck {
inline constexpr std::string_view native_relocation_active = "workspace-relocations/active";
inline constexpr std::string_view native_relocation_archive = "archive/workspace-relocations/";
// Permit only exact journal publication/finalization while a move is pending.
// A live linked registration cannot be rebound through an ordinary ref edit.
void validate_native_workspace_relocation(const Store &, const Refs &, const Refs &);
} // namespace syrnck
