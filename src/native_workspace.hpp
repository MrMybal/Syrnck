#pragma once
#include "internal.hpp"
namespace syrnck {
struct NativeFile {
  fs::path path;
  std::string relative;
  std::uint64_t size;
  fs::file_time_type modified;
  bool executable;
};
// Shared snapshot selection for native commit and working-directory status.
void visit_native_files(const fs::path &directory, const fs::path &store,
                        const std::function<void(const NativeFile &)> &, OperationOptions);
} // namespace syrnck
