#pragma once
#include <syrnck/core.hpp>
#include <algorithm>
namespace syrnck {
inline void validate_native_workspace_name(std::string_view name) {
  require(!name.empty() && name.size() <= 64 && name != "primary" &&
              std::all_of(name.begin(), name.end(), [](char c) {
                return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
              }) && name.front() != '-' && name.back() != '-', "invalid native workspace name");
  require(name != "con" && name != "prn" && name != "aux" && name != "nul" &&
              !(name.size() == 4 && (name.starts_with("com") || name.starts_with("lpt")) &&
                name[3] >= '1' && name[3] <= '9'), "invalid native workspace name");
}
inline std::string native_context_reference(std::string_view context, std::string_view local) {
  validate_ref(local);
  if (context.empty()) return std::string(local);
  validate_native_workspace_name(context);
  return "workspaces/" + std::string(context) + '/' + std::string(local);
}
inline bool native_checkout_active_reference(std::string_view name) {
  if (name == "checkouts/active") return true;
  if (!name.starts_with("workspaces/") || !name.ends_with("/checkouts/active")) return false;
  const auto context = name.substr(11, name.size() - 11 - 17);
  validate_native_workspace_name(context);
  return true;
}
inline bool native_workspace_machine_reference(std::string_view name) {
  if (name == "workspace/HEAD" || name.starts_with("checkouts/receipts/")) return true;
  if (!name.starts_with("workspaces/")) return false;
  const auto suffix = name.substr(11);
  const auto slash = suffix.find('/');
  if (slash == suffix.npos) return false;
  const auto local = suffix.substr(slash + 1);
  if (local != "registration" && local != "workspace/HEAD" && !local.starts_with("checkouts/receipts/")) return false;
  validate_native_workspace_name(suffix.substr(0, slash));
  return true;
}
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
inline std::string native_revision_reference(const Store &store, std::string_view target) {
  const auto name = native_revision_reference(target);
  return native_target_is_detached(target) ? store.native_workspace_reference(name) : name;
}
inline std::string native_staging_reference(std::string_view context, std::string_view target) {
  const auto legacy = native_staging_reference(target);
  if (context.empty()) return legacy;
  if (native_target_is_detached(target)) return native_context_reference(context, legacy);
  const auto id = hash(std::span(reinterpret_cast<const std::uint8_t *>(target.data()), target.size()));
  return native_context_reference(context, "staging/" + id);
}
inline std::string native_staging_reference(const Store &store, std::string_view target) {
  return native_staging_reference(store.native_workspace_name(), target);
}
inline void validate_native_target(std::string_view target) {
  (void)native_revision_reference(target);
  (void)native_staging_reference(target);
}
inline std::string native_revision_selector(std::string_view target) {
  return native_target_is_detached(target) ? std::string(target) : native_revision_reference(target);
}
} // namespace syrnck
