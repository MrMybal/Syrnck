#pragma once
#include "native_renames.hpp"
namespace syrnck {
struct NativeRenameAlignment {
  NativeSnapshot base, current, incoming;
  Json conflicts = Json::array(), report;
  bool applied = false;
};
NativeRenameAlignment align_native_renames(const NativeSnapshot &, const NativeSnapshot &,
                                           const NativeSnapshot &, NativeRenameBatch &, OperationOptions);
} // namespace syrnck
