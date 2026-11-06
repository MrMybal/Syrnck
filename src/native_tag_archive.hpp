#pragma once
#include "internal.hpp"
namespace syrnck {
struct NativeTagArchiveRoots {
  std::string name;
  ObjectKey target;
  Refs roots;
};
// Strict complete group, including descriptor and optional annotation binding.
NativeTagArchiveRoots read_native_tag_archive_roots(const Store &, const Refs &, std::string_view,
                                                    OperationOptions);
} // namespace syrnck
