#pragma once
#include "native_workspace.hpp"
#include "native_text_merge.hpp"
namespace syrnck {
struct NativeRenameLimit : Error {
  using Error::Error;
};
struct NativeRenameMatch {
  std::string target;
  unsigned similarity;
};
struct NativeRenameResult {
  std::map<std::string, NativeRenameMatch> matches;
  std::set<std::string> ambiguous_sources, ambiguous_targets;
  Json report;
};
class NativeRenameBatch {
public:
  using Features = std::map<std::string, std::uint64_t>;
  NativeRenameBatch(const Store &, const NativeRenameOptions &, const NativeGeneratedText * = nullptr,
                    std::function<void(std::size_t)> = {});
  const Features &features(const NativeSnapshotFile &, OperationOptions);
  void work(OperationOptions);
  Json report() const;
  const NativeRenameOptions limits;

private:
  const Store &store_;
  const NativeGeneratedText *generated_;
  std::function<void(std::size_t)> input_observer_;
  std::map<std::string, Features> cache_;
  std::size_t input_bytes_ = 0, work_ = 0;
  void input(std::size_t);
};
NativeRenameResult match_native_renames(const NativeSnapshot &, const NativeSnapshot &, NativeRenameBatch *,
                                        OperationOptions);
} // namespace syrnck
