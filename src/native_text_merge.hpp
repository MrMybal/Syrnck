#pragma once
#include "native_workspace.hpp"
namespace syrnck {
using NativeGeneratedText = std::map<std::string, std::string>;
// Logical IDs are prepared without writing objects. Materialization must finish
// before a candidate revision enters the recoverable checkout transaction.
std::string native_text_manifest(std::string_view, OperationOptions = {});
void materialize_native_text(const Store &, const NativeGeneratedText &, OperationOptions);
class NativeTextMergeBatch {
  const Store &store_;
  TextDiffOptions limits_;
  std::size_t attempted_ = 0, input_bytes_ = 0, result_bytes_ = 0, report_bytes_ = 4096;
  Json rows_ = Json::array();

public:
  static constexpr std::size_t max_files = 64, max_total_bytes = 16 * 1024 * 1024;
  NativeGeneratedText generated;
  NativeTextMergeBatch(const Store &, TextDiffOptions);
  std::optional<NativeSnapshotFile> merge(std::string_view path, const NativeSnapshotFile &base,
                                          const NativeSnapshotFile &current,
                                          const NativeSnapshotFile &incoming, OperationOptions);
  std::size_t rows() const {
    return rows_.size();
  }
  Json summary() const;
  Json report(std::size_t first = 0) const;
};
} // namespace syrnck
