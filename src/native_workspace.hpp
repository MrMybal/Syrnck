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
Json ingest_native_file(const Store &, const NativeFile &, IngestOptions, std::string_view changed_message);
Json compare_native_snapshots(const Store &, const std::optional<std::string> &source,
                              const std::optional<std::string> &target, OperationOptions);
} // namespace syrnck
