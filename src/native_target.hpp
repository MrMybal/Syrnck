#pragma once
#include <syrnck/core.hpp>
namespace syrnck {
// An opaque native-operation selector, deliberately invalid as a branch name.
// The retained revision/index are real private refs, never synthesized heads.
inline constexpr std::string_view detached_native_target = ":HEAD";
inline bool native_target_is_detached(std::string_view target) {
  return target == detached_native_target;
}
inline std::string native_revision_reference(std::string_view target) {
  if (native_target_is_detached(target))
    return "workspace/detached";
  validate_ref(target);
  auto name = "heads/" + std::string(target);
  validate_ref(name);
  return name;
}
inline std::string native_staging_reference(std::string_view target) {
  if (native_target_is_detached(target))
    return "workspace/staging";
  validate_ref(target);
  auto name = "staging/" + std::string(target);
  validate_ref(name);
  return name;
}
inline void validate_native_target(std::string_view target) {
  (void)native_revision_reference(target);
  (void)native_staging_reference(target);
}
inline std::string native_revision_selector(std::string_view target) {
  return native_target_is_detached(target) ? std::string(target) : native_revision_reference(target);
}
} // namespace syrnck
