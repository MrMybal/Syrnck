#pragma once
#include "internal.hpp"
namespace syrnck {
struct NativeDirectoryIdentity {
  std::string volume, object;
  bool operator==(const NativeDirectoryIdentity &) const = default;
  Json json() const { return Json{{"volume", volume}, {"object", object}}; }
};
// Stable across renaming this directory, independent of descendant contents.
// Never follows a link/reparse point at the final path component.
std::optional<NativeDirectoryIdentity> native_directory_identity(const fs::path &);
} // namespace syrnck
