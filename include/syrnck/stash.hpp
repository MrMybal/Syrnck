#pragma once
#include <syrnck/core.hpp>
namespace syrnck {
struct NativeStashSaveOptions {
  bool include_untracked = false;
};
struct NativeStashApplyOptions {
  bool restore_index = false;
  std::optional<NativeResolutionSet> resolutions{}, index_resolutions{};
  std::optional<TextDiffOptions> text_merge{};
};
class NativeStash {
public:
  explicit NativeStash(const Store &store) : store_(store) {}
  Json save(const fs::path &, std::string_view branch, std::string_view message, NativeStashSaveOptions = {},
            OperationOptions = {}) const;
  Json list(const std::optional<std::uint64_t> &before_generation = {}, std::size_t limit = 100,
            OperationOptions = {}) const;
  Json show(std::string_view id, bool verify_objects = false, OperationOptions = {}) const;
  Json drop(std::string_view id, std::string_view expected_record, OperationOptions = {}) const;
  Json archived(std::string_view id, bool verify_objects = false, OperationOptions = {}) const;
  Json restore(std::string_view id, OperationOptions = {}) const;
  Json plan_apply(const fs::path &, std::string_view branch, std::string_view id,
                  const NativeStashApplyOptions & = {}, bool verify_objects = false,
                  OperationOptions = {}) const;
  Json apply(const fs::path &, std::string_view branch, std::string_view id, std::string_view expected_plan,
             bool pop = false, const NativeStashApplyOptions & = {}, OperationOptions = {}) const;

private:
  const Store &store_;
};
} // namespace syrnck
