#pragma once
#include "native_workspace.hpp"
namespace syrnck {
// Internal verified capture, retained only while preparing selected hunks.
struct NativeWorkingDiff {
  Json report;
  std::string source, target;
  std::string path;
  fs::path requested, directory;
  std::optional<NativeFile> file;
};
NativeWorkingDiff read_native_working_diff(const Store &, const fs::path &,
                                           const std::optional<std::string> &, std::string_view,
                                           TextDiffOptions, OperationOptions);
void verify_native_working_diff(const Store &, const NativeWorkingDiff &, OperationOptions);
} // namespace syrnck
