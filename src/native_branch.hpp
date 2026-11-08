#pragma once
#include "internal.hpp"
#include <algorithm>
namespace syrnck {
inline void native_branch_name(std::string_view name) {
  validate_ref(name);
  validate_ref("heads/" + std::string(name));
  validate_ref("staging/" + std::string(name));
}
inline std::optional<ObjectKey> native_branch_lookup(const Refs &refs, std::string_view name) {
  const auto found = refs.find(std::string(name));
  return found == refs.end() ? std::optional<ObjectKey>{} : std::optional{found->second};
}
struct NativeBranchState {
  std::string name, ref, index_ref;
  ObjectKey target;
  std::optional<ObjectKey> index;
  std::optional<std::string> base;
  Json revision, prepared;
  NativeBranchState(const Store &store, const Refs &refs, std::string_view branch, OperationOptions options)
      : name(branch), ref("heads/" + name), index_ref(native_staging_reference(store, name)) {
    throw_if_cancelled(options.stop);
    native_branch_name(name);
    const auto found = native_branch_lookup(refs, ref);
    require(found && found->kind == Kind::revision, "unknown native branch");
    target = *found;
    revision = store.metadata(Kind::revision, target.id);
    index = native_branch_lookup(refs, index_ref);
    if (index) {
      require(index->kind == Kind::revision, "invalid native staging snapshot");
      prepared = store.metadata(Kind::revision, index->id);
      require(prepared["message"] == "Syrnck staging v1: " + name && prepared["timestamp"] == 0 &&
                  prepared["parents"].size() <= 1,
              "invalid native staging snapshot");
      if (!prepared["parents"].empty())
        base = prepared["parents"][0].get<std::string>();
    }
  }
  Json json(bool current) const {
    const auto message = revision["message"].get<std::string>();
    auto end = std::min<std::size_t>(256, message.size());
    if (end < message.size())
      while (end && (static_cast<unsigned char>(message[end]) & 0xc0) == 0x80)
        --end;
    return Json{{"name", name},
                {"revision", target.id},
                {"current", current},
                {"files", revision["files"].size()},
                {"parents", revision["parents"].size()},
                {"timestamp", revision["timestamp"]},
                {"message", message.substr(0, end)},
                {"message_bytes", message.size()},
                {"message_truncated", end != message.size()},
                {"index", index ? Json(index->id) : Json{}},
                {"index_base", base ? Json(*base) : Json{}},
                {"stale_index", index && base != target.id}};
  }
};
} // namespace syrnck
