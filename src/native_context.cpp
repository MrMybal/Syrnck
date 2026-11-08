#include "native_context.hpp"
#include "native_workspace.hpp"
namespace syrnck {
namespace {
std::optional<ObjectKey> lookup(const Refs &refs, std::string_view name) {
  const auto at = refs.find(std::string(name));
  return at == refs.end() ? std::optional<ObjectKey>{} : std::optional{at->second};
}
std::string folded_path(const fs::path &path) {
  auto value = utf8(path.lexically_normal());
  std::replace(value.begin(), value.end(), '\\', '/');
  while (value.size() > 1 && value.back() == '/') value.pop_back();
  return portable_fold(value);
}
bool below(const fs::path &path, const fs::path &parent) {
  const auto p = folded_path(path), base = folded_path(parent);
  return p == base || (p.starts_with(base) && (base.back() == '/' || p[base.size()] == '/'));
}
Json head_value(const Store &store, const ObjectKey &key) {
  require(key.kind == Kind::chunk, "invalid native HEAD");
  const auto data = store.get(key);
  const auto value = parse_json(data);
  require(value.is_object() && value.size() == 4 && value.value("type", "") == "native-head" &&
              value.at("version").is_number_unsigned() &&
              ((value.value("version", 0) == 1 && value.at("branch").is_string()) ||
               (value.value("version", 0) == 2 && value.at("branch").is_null())), "invalid native HEAD");
  if (value["branch"].is_string()) validate_ref(value["branch"].get<std::string>());
  const auto path = path_from_utf8(value.at("directory").get<std::string>());
  const auto canonical = Json{{"version", value["branch"].is_null() ? 2 : 1}, {"type", "native-head"},
      {"directory", utf8(path)}, {"branch", value["branch"]}};
  require(path.is_absolute() && path == path.lexically_normal() && canonical.dump() == text(data),
          "invalid native HEAD");
  return value;
}
NativeWorkspaceContext context(const Store &store, const Refs &refs, std::string_view name) {
  validate_native_workspace_name(name);
  const auto key = lookup(refs, native_context_reference(name, "registration"));
  require(key && key->kind == Kind::chunk, "unknown native workspace");
  const auto data = store.get(*key);
  const auto value = parse_json(data);
  require(value.is_object() && value.size() == 4 && value.value("version", 0) == 1 &&
              value.value("type", "") == "native-workspace" && value.at("name") == Json(name),
          "invalid native workspace registration");
  NativeWorkspaceContext result{std::string(name), path_from_utf8(value.at("directory").get<std::string>()), *key};
  const auto canonical = Json{{"version", 1}, {"type", "native-workspace"}, {"name", name},
      {"directory", utf8(result.directory)}};
  require(result.directory.is_absolute() && result.directory == result.directory.lexically_normal() &&
              !below(result.directory, store.root()) && !below(store.root(), result.directory) &&
              canonical.dump() == text(data), "invalid native workspace registration");
  return result;
}
std::vector<std::string> names(const Refs &refs) {
  std::vector<std::string> result;
  for (auto at = refs.lower_bound("workspaces/"); at != refs.end() && at->first.starts_with("workspaces/"); ++at) {
    const auto suffix = std::string_view(at->first).substr(11);
    const auto slash = suffix.find('/');
    if (slash == suffix.npos || suffix.substr(slash + 1) != "registration") continue;
    validate_native_workspace_name(suffix.substr(0, slash));
    require(result.size() < 1024, "native workspace registration limit exceeded");
    result.emplace_back(suffix.substr(0, slash));
  }
  return result;
}
} // namespace
NativeWorkspaceContext read_native_workspace_context(const Store &store, std::string_view name) {
  const auto result = context(store, store.refs(), name);
  const auto status = fs::symlink_status(result.directory);
  require(fs::is_directory(status) && !native_link_like(result.directory, status) &&
              fs::canonical(result.directory) == result.directory, "native workspace directory changed");
  return result;
}
std::optional<std::string> native_branch_workspace_owner(const Store &store, std::string_view branch) {
  if (native_target_is_detached(branch)) return {};
  const auto refs = store.refs();
  const auto primary = lookup(refs, "workspace/HEAD");
  const auto selected = primary ? head_value(store, *primary)["branch"] : Json("main");
  if (selected == Json(branch)) return std::string{};
  for (const auto &name : names(refs)) {
    const auto key = lookup(refs, native_context_reference(name, "workspace/HEAD"));
    require(key.has_value(), "invalid native workspace registration");
    if (head_value(store, *key)["branch"] == Json(branch)) return name;
  }
  return {};
}
void require_native_branch_available(const Store &store, std::string_view branch) {
  const auto owner = native_branch_workspace_owner(store, branch);
  require(!owner || *owner == store.native_workspace_name(), "native branch is selected in another workspace");
}
void Store::validate_native_workspace(const fs::path &directory) const {
  const auto root = fs::canonical(directory);
  require(!below(root, this->root()), "native store must be below checkout workspace");
  if (native_workspace_name().empty()) {
    require(below(this->root(), root), "native store must be below checkout workspace");
    const auto refs = this->refs();
    if (!names(refs).empty()) {
      const auto head = lookup(refs, "workspace/HEAD");
      require(head && head_value(*this, *head)["directory"] == utf8(root), "native HEAD belongs to another workspace");
    }
  } else
    require(read_native_workspace_context(*this, native_workspace_name()).directory == root,
            "native HEAD belongs to another workspace");
}
void validate_native_branch_reservations(const Store &store, const Refs &before, const Refs &after) {
  const auto contexts = names(after);
  if (contexts.empty() && names(before).empty()) return;
  std::map<std::string, std::string> owners;
  std::vector<std::string> directories;
  auto reserve = [&](std::string_view name, const std::optional<ObjectKey> &key) {
    if (!key) return;
    const auto head = head_value(store, *key);
    if (head["branch"].is_null()) return;
    const auto branch = head["branch"].get<std::string>();
    require(owners.emplace(branch, std::string(name)).second, "native branch is selected in another workspace");
  };
  const auto primary = lookup(after, "workspace/HEAD");
  require(contexts.empty() || primary.has_value(), "invalid native workspace registration");
  if (primary) {
    const auto directory = path_from_utf8(head_value(store, *primary)["directory"].get<std::string>());
    require(directory != store.root() && below(store.root(), directory), "invalid native HEAD");
    directories.push_back(folded_path(directory));
  }
  reserve("", primary);
  for (const auto &name : contexts) {
    const auto entry = context(store, after, name);
    directories.push_back(folded_path(entry.directory));
    const auto head = lookup(after, native_context_reference(name, "workspace/HEAD"));
    require(head.has_value() && head_value(store, *head)["directory"] == utf8(entry.directory),
            "invalid native workspace registration");
    reserve(name, head);
  }
  const std::set<std::string> unique(directories.begin(), directories.end());
  require(unique.size() == directories.size(), "native workspaces must not overlap");
  for (const auto &path : directories)
    for (auto slash = path.find('/'); slash != path.npos; slash = path.find('/', slash + 1))
      require(!unique.contains(slash ? path.substr(0, slash) : std::string("/")) || path == "/",
              "native workspaces must not overlap");
  // Even a transaction that releases another workspace's selection must not
  // rewrite its branch behind its working files. Release it in that context.
  const auto previous = names(before);
  auto protect = [&](std::string_view name, const std::optional<ObjectKey> &key) {
    if (name == store.native_workspace_name() || !key) return;
    const auto head = head_value(store, *key);
    if (head["branch"].is_null()) return;
    const auto reference = "heads/" + head["branch"].get<std::string>();
    require(lookup(before, reference) == lookup(after, reference),
            "native branch is selected in another workspace");
  };
  protect("", lookup(before, "workspace/HEAD"));
  for (const auto &name : previous)
    protect(name, lookup(before, native_context_reference(name, "workspace/HEAD")));
}
Json Store::register_native_workspace(const fs::path &primary, std::string_view name,
                                      const fs::path &directory, OperationOptions options) const {
  throw_if_cancelled(options.stop);
  require(native_workspace_name().empty(), "workspace registration requires the primary context");
  require_no_native_checkout(*this);
  validate_native_workspace_name(name);
  validate_native_workspace(primary);
  const auto primary_root = fs::canonical(primary);
  const auto selected = native_head();
  require(selected["directory"].is_null() || selected["directory"] == utf8(primary_root),
          "native HEAD belongs to another workspace");
  auto target = fs::absolute(directory).lexically_normal();
  require(target != target.root_path() && !below(target, root()) && !below(root(), target) &&
              !below(target, primary_root) && !below(primary_root, target), "native workspaces must not overlap");
  const auto refs = this->refs();
  const auto prefix = native_context_reference(name, "registration");
  const auto reserved = "workspaces/" + std::string(name) + '/';
  const auto existing = refs.lower_bound(reserved);
  require(existing == refs.end() || !existing->first.starts_with(reserved), "native workspace name already exists");
  const auto registered = names(refs);
  require(registered.size() < 1024, "native workspace registration limit exceeded");
  // Resolve existing ancestors before comparing roots, including junctions.
  target = fs::exists(target) ? fs::canonical(target) : fs::canonical(target.parent_path()) / target.filename();
  require(!below(target, primary_root) && !below(primary_root, target) &&
              !below(target, root()) && !below(root(), target), "native workspaces must not overlap");
  for (const auto &other : registered) {
    const auto entry = context(*this, refs, other);
    require(!below(target, entry.directory) && !below(entry.directory, target), "native workspaces must not overlap");
  }
  const auto status = fs::symlink_status(target);
  if (status.type() == fs::file_type::not_found) {
    require(fs::create_directory(target), "cannot create native workspace directory");
    sync_directory(target.parent_path());
  } else
    require(fs::is_directory(status) && !native_link_like(target, status), "invalid native workspace directory");
  target = fs::canonical(target);
  require_features({"linked-workspaces-v1"}, options);
  const auto registration = put(Kind::chunk, bytes(Json{{"version", 1}, {"type", "native-workspace"},
      {"name", name}, {"directory", utf8(target)}}.dump())).first;
  const auto head = put(Kind::chunk, bytes(Json{{"version", 2}, {"type", "native-head"},
      {"directory", utf8(target)}, {"branch", Json{}}}.dump())).first;
  const auto initial = put(Kind::revision, bytes(Json{{"version", 1}, {"parents", Json::array()},
      {"message", "Syrnck workspace initialization v1"}, {"timestamp", 0}, {"files", Json::object()}}.dump())).first;
  const auto primary_head = lookup(refs, "workspace/HEAD");
  auto bound_primary = primary_head;
  if (!bound_primary)
    bound_primary = put(Kind::chunk, bytes(Json{{"version", 1}, {"type", "native-head"},
        {"directory", utf8(primary_root)}, {"branch", "main"}}.dump())).first;
  require(compare_update_refs({{prefix, {}, registration},
      {native_context_reference(name, "workspace/HEAD"), {}, head},
      {native_context_reference(name, "workspace/detached"), {}, initial},
      {"workspace/HEAD", primary_head, bound_primary}}, options), "native workspace references changed; retry operation");
  return Json{{"version", 1}, {"state", "registered"}, {"name", name}, {"directory", utf8(target)},
      {"branch", Json{}}, {"revision", initial.id}};
}
Json Store::restore_native_workspace(const fs::path &primary, std::string_view name, const fs::path &directory,
                                      NativeWorkspaceRestoreOptions settings, OperationOptions options) const {
  throw_if_cancelled(options.stop);
  require(native_workspace_name().empty(), "workspace restoration requires the primary context");
  require_no_native_checkout(*this);
  validate_native_workspace_name(name);
  require(!(settings.branch && settings.detached), "choose a named or detached workspace restoration");
  validate_native_workspace(primary);
  const auto primary_root = fs::canonical(primary);
  const auto selected = native_head();
  require(selected["directory"].is_null() || selected["directory"] == utf8(primary_root), "native HEAD belongs to another workspace");
  const auto refs = this->refs();
  const auto registration_name = native_context_reference(name, "registration");
  const auto head_name = native_context_reference(name, "workspace/HEAD");
  require(!lookup(refs, registration_name) && !lookup(refs, head_name), "native workspace name already exists");
  const auto portable_name = native_context_reference(name, "portable"), snapshot_name = native_context_reference(name, "snapshot");
  const auto descriptor = lookup(refs, portable_name), snapshot = lookup(refs, snapshot_name);
  require(descriptor && descriptor->kind == Kind::chunk && snapshot && snapshot->kind == Kind::revision,
          "unknown portable native workspace");
  const auto data = get(*descriptor);
  const auto value = parse_json(data);
  require(value.is_object() && value.size() == 5 && value.value("version", 0) == 1 &&
              value.value("type", "") == "native-workspace-snapshot" && value.at("name") == Json(name) &&
              value.at("revision") == snapshot->id && (value.at("branch").is_null() || value.at("branch").is_string()),
          "invalid portable native workspace");
  const auto canonical = Json{{"version", 1}, {"type", "native-workspace-snapshot"}, {"name", name},
      {"branch", value["branch"]}, {"revision", snapshot->id}};
  require(canonical.dump() == text(data), "invalid portable native workspace");
  const auto original_target = value["branch"].is_null() ? std::string(detached_native_target) : value["branch"].get<std::string>();
  if (!value["branch"].is_null()) validate_ref(original_target);
  auto target = settings.detached ? std::string(detached_native_target) : settings.branch.value_or(original_target);
  validate_native_target(target);
  if (settings.branch) validate_ref(*settings.branch);
  std::optional<ObjectKey> branch_tip;
  if (!native_target_is_detached(target)) {
    require(!native_branch_workspace_owner(*this, target), "native branch is selected in another workspace");
    branch_tip = lookup(refs, native_revision_reference(target));
    require(!branch_tip || branch_tip == snapshot, "portable workspace branch changed; choose another branch or detach");
  }
  auto destination = fs::absolute(directory).lexically_normal();
  require(destination != destination.root_path(), "invalid native workspace directory");
  const auto status = fs::symlink_status(destination);
  const bool exists = status.type() != fs::file_type::not_found;
  if (exists)
    require(fs::is_directory(status) && !native_link_like(destination, status), "invalid native workspace directory");
  destination = exists ? fs::canonical(destination) : fs::canonical(destination.parent_path()) / destination.filename();
  require(!below(destination, primary_root) && !below(primary_root, destination) &&
              !below(destination, root()) && !below(root(), destination), "native workspaces must not overlap");
  const auto registered = names(refs);
  require(registered.size() < 1024, "native workspace registration limit exceeded");
  for (const auto &other : registered) {
    const auto entry = context(*this, refs, other);
    require(!below(destination, entry.directory) && !below(entry.directory, destination), "native workspaces must not overlap");
  }
  const auto original_index_name = native_staging_reference(name, original_target);
  const auto next_index_name = native_staging_reference(name, target);
  const auto original_index = lookup(refs, original_index_name);
  auto next_index = original_index;
  Json index;
  if (original_index) {
    require(original_index->kind == Kind::revision, "invalid native staging snapshot");
    index = metadata(Kind::revision, original_index->id);
    require(index["message"] == "Syrnck staging v1: " + original_target && index["timestamp"] == 0 &&
                index["parents"].size() <= 1, "invalid native staging snapshot");
  }
  std::vector<RefUpdate> updates{{portable_name, descriptor, descriptor}, {snapshot_name, snapshot, snapshot}};
  if (original_index_name != next_index_name) {
    require(!lookup(refs, next_index_name), "portable workspace destination index is occupied");
    if (original_index) {
      index["message"] = "Syrnck staging v1: " + target;
      next_index = put(Kind::revision, bytes(index.dump())).first;
      const auto archive = "archive/reset-index/" + original_index->id;
      const auto previous = lookup(refs, archive);
      require(!previous || previous == original_index, "native workspace archive reference is occupied");
      updates.push_back({archive, previous, original_index});
    }
    updates.push_back({original_index_name, original_index, {}});
    updates.push_back({next_index_name, {}, next_index});
  } else updates.push_back({original_index_name, original_index, original_index});
  if (native_target_is_detached(target)) {
    const auto slot = native_context_reference(name, "workspace/detached");
    const auto previous = lookup(refs, slot);
    if (previous && previous != snapshot) {
      require(previous->kind == Kind::revision, "invalid native workspace retained revision");
      const auto archive = "archive/" + previous->id;
      const auto old = lookup(refs, archive);
      require(!old || old == previous, "native workspace archive reference is occupied");
      updates.push_back({archive, old, previous});
    }
    updates.push_back({slot, previous, snapshot});
  } else updates.push_back({native_revision_reference(target), branch_tip, snapshot});
  const auto materialized = next_index ? next_index->id : snapshot->id;
  require_features({"linked-workspaces-v1"}, options);
  if (!exists) export_revision(materialized, destination, options);
  const auto registration = put(Kind::chunk, bytes(Json{{"version", 1}, {"type", "native-workspace"},
      {"name", name}, {"directory", utf8(destination)}}.dump())).first;
  const auto head = put(Kind::chunk, bytes(Json{{"version", native_target_is_detached(target) ? 2 : 1}, {"type", "native-head"},
      {"directory", utf8(destination)}, {"branch", native_target_is_detached(target) ? Json{} : Json(target)}}.dump())).first;
  updates.push_back({registration_name, {}, registration});
  updates.push_back({head_name, {}, head});
  const auto primary_head = lookup(refs, "workspace/HEAD");
  auto bound_primary = primary_head;
  if (!bound_primary)
    bound_primary = put(Kind::chunk, bytes(Json{{"version", 1}, {"type", "native-head"},
        {"directory", utf8(primary_root)}, {"branch", "main"}}.dump())).first;
  updates.push_back({"workspace/HEAD", primary_head, bound_primary});
  report_progress(options, {ProgressPhase::publishing, "workspace-restore"});
  verify_native_restored_directory(*this, destination, materialized, {options.stop});
  require(compare_update_refs(updates, {options.stop}), "native workspace references changed; retry operation");
  return Json{{"version", 1}, {"state", "restored"}, {"name", name}, {"directory", utf8(destination)},
      {"branch", native_target_is_detached(target) ? Json{} : Json(target)}, {"revision", snapshot->id},
      {"index", next_index ? Json(next_index->id) : Json{}}, {"materialized_revision", materialized}, {"reused_directory", exists}};
}
Json Store::native_workspaces(OperationOptions options) const {
  const auto refs = this->refs();
  Json rows = Json::array();
  const auto primary_key = lookup(refs, "workspace/HEAD");
  const auto primary = primary_key ? head_value(*this, *primary_key) : Json{{"directory", Json{}}, {"branch", "main"}};
  const auto primary_target = primary["branch"].is_null() ? std::string("workspace/detached")
      : "heads/" + primary["branch"].get<std::string>();
  const auto primary_revision = lookup(refs, primary_target);
  rows.push_back(Json{{"name", "primary"}, {"directory", primary["directory"]}, {"branch", primary["branch"]},
      {"revision", primary_revision ? Json(primary_revision->id) : Json{}}, {"selected", native_workspace_name().empty()},
      {"pending_checkout", refs.contains("checkouts/active")}});
  auto linked = native_linked_workspace_rows(*this, refs, options);
  for (auto &row : linked) rows.push_back(std::move(row));
  return Json{{"version", 1}, {"workspaces", rows},
      {"selected", native_workspace_name().empty() ? "primary" : std::string(native_workspace_name())}};
}
Json native_linked_workspace_rows(const Store &store, const Refs &refs, OperationOptions options) {
  Json rows = Json::array();
  for (const auto &name : names(refs)) {
    throw_if_cancelled(options.stop);
    const auto entry = context(store, refs, name);
    const auto key = lookup(refs, native_context_reference(name, "workspace/HEAD"));
    require(key.has_value(), "invalid native workspace registration");
    const auto head = head_value(store, *key);
    const auto target = head["branch"].is_null() ? native_context_reference(name, "workspace/detached")
        : "heads/" + head["branch"].get<std::string>();
    const auto revision = lookup(refs, target);
    rows.push_back(Json{{"name", name}, {"directory", utf8(entry.directory)}, {"branch", head["branch"]},
        {"revision", revision ? Json(revision->id) : Json{}}, {"selected", name == store.native_workspace_name()},
        {"pending_checkout", refs.contains(native_context_reference(name, "checkouts/active"))}});
  }
  return rows;
}
fs::path native_context_local_path(const fs::path &storage, std::string_view context, std::string_view leaf) {
  validate_ref(leaf);
  require(leaf.find('/') == leaf.npos, "invalid native workspace local filename");
  if (context.empty()) return storage / path_from_utf8(leaf);
  validate_native_workspace_name(context);
  auto directory = storage;
  for (const auto &component : {std::string("workspaces"), std::string(context)}) {
    directory /= path_from_utf8(component);
    const auto status = fs::symlink_status(directory);
    if (status.type() == fs::file_type::not_found) {
      require(fs::create_directory(directory), "cannot create native workspace directory");
#ifndef _WIN32
      fs::permissions(directory, fs::perms::owner_all);
#endif
      sync_directory(directory.parent_path());
    } else
      require(fs::is_directory(status) && !native_link_like(directory, status), "invalid native workspace directory");
  }
  return directory / path_from_utf8(leaf);
}
fs::path native_workspace_local_path(const Store &store, std::string_view leaf) {
  return native_context_local_path(store.root(), store.native_workspace_name(), leaf);
}
} // namespace syrnck
