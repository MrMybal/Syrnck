#pragma once
#include "native_workspace.hpp"
namespace syrnck {
struct NativeRewriteState {
  Refs refs;
  std::string branch, head_name, index_name;
  ObjectKey head;
  std::optional<ObjectKey> index;
  Json revision, prepared;
  std::optional<std::string> index_base;
  NativeRewriteState(const Store &store, std::string_view name, OperationOptions options)
      : refs(store.refs()), branch(name), head_name("heads/" + branch), index_name("staging/" + branch) {
    throw_if_cancelled(options.stop);
    validate_ref(head_name);
    validate_ref(index_name);
    const auto found = refs.find(head_name);
    require(found != refs.end() && found->second.kind == Kind::revision, "unknown native branch");
    head = found->second;
    revision = store.metadata(Kind::revision, head.id);
    index = lookup(index_name);
    if (index) {
      require(index->kind == Kind::revision, "invalid native staging snapshot");
      prepared = store.metadata(Kind::revision, index->id);
      require(prepared["message"] == "Syrnck staging v1: " + branch && prepared["timestamp"] == 0 &&
                  prepared["parents"].size() <= 1,
              "invalid native staging snapshot");
      if (!prepared["parents"].empty())
        index_base = prepared["parents"][0].get<std::string>();
    }
  }
  std::optional<ObjectKey> lookup(const std::string &name) const {
    const auto found = refs.find(name);
    return found == refs.end() ? std::optional<ObjectKey>{} : std::optional{found->second};
  }
  std::string archive(std::string_view kind) const {
    return "archive/" + std::string(kind) + '/' + head.id;
  }
  std::string index_archive() const {
    return "archive/reset-index/" + index->id;
  }
  void check_archive(const std::string &name, const ObjectKey &key, Json &blockers) const {
    const auto existing = lookup(name);
    if (existing && *existing != key)
      blockers.push_back(Json{{"reason", "archive_conflict"}, {"reference", name}});
  }
  void publish(const Store &store, std::string_view kind, const ObjectKey &target,
               const std::optional<ObjectKey> &next_index, OperationOptions options) const {
    const auto name = archive(kind);
    std::vector<RefUpdate> updates{
        {head_name, head, target}, {index_name, index, next_index}, {name, lookup(name), head}};
    if (index) {
      const auto saved = index_archive();
      updates.push_back({saved, lookup(saved), index});
    }
    require(store.compare_update_refs(updates, options),
            "native history references changed; retry operation");
  }
};
} // namespace syrnck
