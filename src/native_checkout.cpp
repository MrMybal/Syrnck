#include "native_workspace.hpp"
#include "native_checkout_layout.hpp"
#include "native_hard_reset.hpp"
#include "native_rewrite.hpp"
#include <tuple>
#ifdef _WIN32
#include <windows.h>
#endif
namespace syrnck {
namespace {
enum class Type { file, directory, symlink, special, protected_path };
struct Node {
  Type type;
  std::uint64_t size = 0;
  fs::file_time_type modified{};
  bool executable = false;
};
bool inside(const fs::path &path, const fs::path &root) {
  auto p = path.begin();
  for (auto r = root.begin(); r != root.end(); ++r, ++p)
    if (p == path.end() || *p != *r)
      return false;
  return true;
}
} // namespace
bool native_link_like(const fs::path &path, const fs::file_status &status) {
#ifdef _WIN32
  const auto attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    const auto error = GetLastError();
    const auto detail = utf8(path) + " [" + std::to_string(error) + "]";
    throw Error("cannot inspect native checkout entry: " + detail);
  }
  if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
    return true;
#else
  (void)path;
#endif
  return fs::is_symlink(status);
}
namespace {
using Conflicts = std::set<std::tuple<std::string, std::string, std::string>>;
class Inventory {
  using Nodes = std::map<std::string, Node>;
  Nodes nodes_;
  std::map<std::string, std::vector<const Nodes::value_type *>> folded_;

public:
  fs::path base;
  Inventory(const fs::path &directory, const Store &storage, const OperationOptions &options, bool restored = false)
      : base(fs::canonical(directory)) {
    if (!restored) storage.validate_native_workspace(base);
    else require(base != storage.root() && !inside(base, storage.root()) && !inside(storage.root(), base),
                  "native workspaces must not overlap");
    const auto &store = storage.root();
    std::vector<std::pair<fs::path, unsigned>> pending{{base, 0}};
    std::size_t budget = 0;
    while (!pending.empty()) {
      throw_if_cancelled(options.stop);
      auto [parent, depth] = std::move(pending.back());
      pending.pop_back();
      report_progress(options,
                      {ProgressPhase::scanning, utf8(parent.lexically_relative(base)), nodes_.size()});
      for (const auto &entry : fs::directory_iterator(parent)) {
        throw_if_cancelled(options.stop);
        const auto relative = utf8(entry.path().lexically_relative(base));
        const auto folded = portable_fold(relative);
        const auto cost = relative.size() + folded.size() + 64;
        require(cost <= max_object_size - budget, "native checkout inventory exceeds 16 MiB budget");
        budget += cost;
        const auto name = portable_fold(utf8(entry.path().filename()));
        const auto status = entry.symlink_status();
        Node node{Type::special};
        if (name == ".git" || name == ".syrnck" || inside(entry.path(), store))
          node.type = Type::protected_path;
        else if (native_link_like(entry.path(), status))
          node.type = Type::symlink;
        else if (fs::is_directory(status)) {
          require(depth < 128, "native checkout directory nesting exceeds 128 levels");
          node.type = Type::directory;
          pending.emplace_back(entry.path(), depth + 1);
        } else if (fs::is_regular_file(status)) {
          node.type = Type::file;
          node.size = entry.file_size();
          node.modified = entry.last_write_time();
#ifndef _WIN32
          node.executable = (status.permissions() & (fs::perms::owner_exec | fs::perms::group_exec |
                                                     fs::perms::others_exec)) != fs::perms::none;
#endif
        }
        const auto found = nodes_.emplace(relative, node).first;
        folded_[folded].push_back(&*found);
      }
    }
    report_progress(options, {ProgressPhase::scanning, {}, nodes_.size(), nodes_.size(), true});
  }
  std::size_t size() const {
    return nodes_.size();
  }
  const Nodes &nodes() const {
    return nodes_;
  }
  const Node *find(const std::string &path) const {
    const auto found = nodes_.find(path);
    return found == nodes_.end() ? nullptr : &found->second;
  }
  // Report namespace obstructions before opening anything. Links/reparse
  // points are opaque, including inside ignored directories.
  bool inspect(const std::string &path, Conflicts &conflicts, bool leaf,
               const std::set<std::string> *evacuated = nullptr) const {
    const auto folded = folded_.find(portable_fold(path));
    if (folded == folded_.end())
      return true;
    if (folded->second.size() != 1) {
      for (const auto *node : folded->second)
        conflicts.emplace(path, "portable_collision", node->first);
      return false;
    }
    const auto &[actual, node] = *folded->second.front();
    if (evacuated && evacuated->contains(actual) && (node.type == Type::file || node.type == Type::directory))
      return true;
    if (actual != path) {
      conflicts.emplace(path, "path_spelling", actual);
      return false;
    }
    const auto reason = node.type == Type::protected_path                    ? "protected_path"
                        : node.type == Type::symlink                         ? "symlink_obstruction"
                        : node.type == Type::special                         ? "special_file"
                        : node.type == (leaf ? Type::directory : Type::file) ? "file_directory_transition"
                                                                             : nullptr;
    if (reason) {
      conflicts.emplace(path, reason, actual);
      return false;
    }
    return true;
  }
  bool parents(const std::string &path, Conflicts &conflicts,
               const std::set<std::string> *evacuated = nullptr) const {
    bool valid = true;
    for (auto n = path.find('/'); n != path.npos; n = path.find('/', n + 1))
      if (!inspect(path.substr(0, n), conflicts, false, evacuated)) {
        valid = false;
        break;
      }
    return valid;
  }
  void evacuation(const NativeCheckoutLayout &layout, Conflicts &conflicts, OperationOptions options) const {
    for (const auto &[path, node] : nodes_) {
      throw_if_cancelled(options.stop);
      bool removed = layout.remove_directories.contains(path);
      for (auto n = path.find('/'); !removed && n != path.npos; n = path.find('/', n + 1))
        removed = layout.remove_directories.contains(path.substr(0, n));
      if (removed &&
          (!layout.evacuated.contains(path) || (node.type != Type::file && node.type != Type::directory)))
        conflicts.emplace(path, "untracked_collision", path);
    }
  }
  bool removed(const std::string &path, const std::set<std::string> &targets,
               const std::set<std::string> &directories) const {
    for (auto n = path.find('/');; n = path.find('/', n + 1)) {
      const auto prefix = n == path.npos ? path : path.substr(0, n);
      const auto found = folded_.find(portable_fold(prefix));
      if (found == folded_.end())
        return true;
      if (found->second.size() != 1)
        return false;
      const auto &[actual, node] = *found->second.front();
      if (node.type == Type::file)
        return targets.contains(actual);
      if (node.type != Type::directory || (actual != prefix && !directories.contains(actual)))
        return false;
      if (n == path.npos)
        return directories.contains(actual);
    }
  }
};
bool executable_equal(bool actual, bool expected) {
#ifdef _WIN32
  (void)actual;
  (void)expected;
  return true;
#else
  return actual == expected;
#endif
}
} // namespace
void verify_native_restored_directory(const Store &store, const fs::path &directory, std::string_view revision,
                                      OperationOptions options, const NativeMaterialization &materialization) {
  const auto expected = read_native_materialized_snapshot(store, revision, materialization, options);
  const Inventory inventory(directory, store, options, true);
  Bytes buffer(1024 * 1024);
  std::size_t files = 0;
  Conflicts conflicts;
  for (const auto &[path, node] : inventory.nodes()) {
    throw_if_cancelled(options.stop);
    if (node.type == Type::directory) continue;
    const auto wanted = expected.find(path);
    require(node.type == Type::file && wanted != expected.end() && inventory.parents(path, conflicts) &&
                inventory.inspect(path, conflicts, true), "restored workspace does not match its snapshot");
    const auto content = hash_native_file({inventory.base / path_from_utf8(path), path, node.size, node.modified, node.executable},
                                           buffer, options);
    require(content == wanted->second.content && executable_equal(node.executable, wanted->second.executable),
            "restored workspace does not match its snapshot");
    ++files;
  }
  require(files == expected.size(), "restored workspace does not match its snapshot");
}
void verify_native_checkout_namespace(const Store &store, const fs::path &directory,
                                      const std::vector<std::string> &target,
                                      const std::vector<std::string> &removed, OperationOptions options,
                                      bool final, bool transitions) {
  const Inventory inventory(directory, store, options);
  Conflicts conflicts;
  for (const auto &path : target) {
    throw_if_cancelled(options.stop);
    require(inventory.parents(path, conflicts) && inventory.inspect(path, conflicts, true) &&
                (!final || inventory.find(path)),
            "native checkout path became obstructed");
  }
  std::set<std::string> targets(target.begin(), target.end()), directories;
  if (transitions)
    for (const auto &path : target)
      for (auto n = path.find('/'); n != path.npos; n = path.find('/', n + 1))
        directories.insert(path.substr(0, n));
  for (const auto &path : removed) {
    throw_if_cancelled(options.stop);
    if (transitions) {
      require(inventory.removed(path, targets, directories), "native checkout path became obstructed");
      continue;
    }
    require(inventory.parents(path, conflicts) && inventory.inspect(path, conflicts, true) &&
                (!final || !inventory.find(path)),
            "native checkout path became obstructed");
  }
}
namespace {
Json plan_checkout_impl(const Store &store, const fs::path &directory, std::string_view branch,
                        const NativeSnapshot *supplied, std::optional<std::string_view> target,
                        bool verify_objects, OperationOptions options,
                        const NativeSnapshot *source_override = nullptr,
                        const std::optional<std::string> &override_id = {},
                        const std::set<std::string> &source_directories = {},
                        const Inventory *supplied_inventory = nullptr) {
  throw_if_cancelled(options.stop);
  auto staging = store.staged_status(branch, options);
  const auto materialization = read_native_materialization(store).policy;
  const auto source_id = source_override             ? override_id
                         : staging["head"].is_null() ? std::optional<std::string>{}
                                                     : std::optional{staging["head"].get<std::string>()};
  const auto stored_source = source_override ? NativeSnapshot{}
                             : source_id     ? read_native_materialized_snapshot(store, *source_id, materialization, options)
                                             : NativeSnapshot{};
  const auto &source = source_override ? *source_override : stored_source;
  const bool same = target && source_id && *source_id == *target;
  const auto other = supplied || same ? NativeSnapshot{} : read_native_materialized_snapshot(store, *target, materialization, options);
  NativeSnapshot projected;
  if (supplied && !materialization.complete()) {
    projected = *supplied;
    project_native_materialization(projected, materialization);
  }
  const auto &destination = supplied ? (materialization.complete() ? *supplied : projected) : same ? source : other;
  const NativeCheckoutLayout layout(source, destination, options, source_directories);
  const auto owned_inventory =
      supplied_inventory ? std::optional<Inventory>{}
                         : std::optional<Inventory>{std::in_place, directory, store, options};
  const auto &inventory = supplied_inventory ? *supplied_inventory : *owned_inventory;
  Conflicts conflicts;
  const auto owner = native_branch_workspace_owner(store, branch);
  if (owner && *owner != store.native_workspace_name())
    conflicts.emplace("", "branch_in_other_workspace", owner->empty() ? "primary" : *owner);
  const auto refs = store.refs();
  if (std::any_of(refs.begin(), refs.end(), [](const auto &entry) { return native_checkout_active_reference(entry.first); }))
    conflicts.emplace("", "checkout_pending", "");
  if (!source_override && !staging["snapshot"].is_null())
    conflicts.emplace("", staging["stale"].get<bool>() ? "stale_staging" : "staging_present", "");
  Bytes buffer(1024 * 1024);
  std::uint64_t hashed_bytes = 0;
  // This first profile deliberately requires every tracked file to be clean,
  // even when the target would leave that file untouched. Ignore rules do not
  // hide tracked edits or untracked collisions from the checkout preflight.
  for (const auto &[path, entry] : source) {
    throw_if_cancelled(options.stop);
    if (!inventory.parents(path, conflicts) || !inventory.inspect(path, conflicts, true))
      continue;
    const auto *node = inventory.find(path);
    if (!node) {
      conflicts.emplace(path, "tracked_missing", path);
      continue;
    }
    const auto content = supplied_inventory ? entry.content
                                            : hash_native_file({inventory.base / path_from_utf8(path), path,
                                                                node->size, node->modified, node->executable},
                                                               buffer, options);
    require(content.size <= UINT64_MAX - hashed_bytes, "native comparison byte count overflow");
    hashed_bytes += content.size;
    if (content != entry.content)
      conflicts.emplace(path, "tracked_modified", path);
    if (!executable_equal(node->executable, entry.executable))
      conflicts.emplace(path, "tracked_mode_changed", path);
  }
  std::set<std::string> directories;
  if (layout.transition)
    inventory.evacuation(layout, conflicts, options);
  const auto *evacuated = layout.transition ? &layout.evacuated : nullptr;
  for (const auto &[path, entry] : destination) {
    (void)entry;
    throw_if_cancelled(options.stop);
    if (!inventory.parents(path, conflicts, evacuated) ||
        !inventory.inspect(path, conflicts, true, evacuated))
      continue;
    if (inventory.find(path) && !source.contains(path) && !layout.remove_directories.contains(path))
      conflicts.emplace(path, "untracked_collision", path);
    for (auto n = path.find('/'); n != path.npos; n = path.find('/', n + 1)) {
      auto parent = path.substr(0, n);
      if (!inventory.find(parent) || (evacuated && evacuated->contains(parent)))
        directories.insert(std::move(parent));
    }
  }
  Json actions = Json::array();
  std::map<std::string, std::uint64_t> counts{{"create", 0},         {"replace", 0}, {"remove", 0},
                                              {"set_executable", 0}, {"reuse", 0},   {"unchanged", 0}};
  std::uint64_t restore_bytes = 0;
  auto left = source.begin(), right = destination.begin();
  const auto total = source.size() + destination.size();
  std::uint64_t processed = 0;
  report_progress(options, {ProgressPhase::planning, {}, 0, total});
  while (left != source.end() || right != destination.end()) {
    throw_if_cancelled(options.stop);
    const bool before = left != source.end() && (right == destination.end() || left->first <= right->first);
    const bool after = right != destination.end() && (left == source.end() || right->first <= left->first);
    const auto &path = before ? left->first : right->first;
    const auto *old = before ? &left->second : nullptr, *next = after ? &right->second : nullptr;
    const auto action = !old                                  ? "create"
                        : !next                               ? "remove"
                        : old->content != next->content       ? "replace"
                        : old->executable != next->executable ? "set_executable"
                        : old->manifest != next->manifest     ? "reuse"
                                                              : "unchanged";
    ++counts.at(action);
    if (std::string_view(action) != "unchanged") {
      actions.push_back(Json{{"path", path},
                             {"action", action},
                             {"source", old ? old->json() : Json{}},
                             {"target", next ? next->json() : Json{}}});
      if (std::string_view(action) == "create" || std::string_view(action) == "replace") {
        require(next->content.size <= UINT64_MAX - restore_bytes, "native checkout byte count overflow");
        restore_bytes += next->content.size;
      }
    }
    processed += before + after;
    if (processed % 256 == 0)
      report_progress(options, {ProgressPhase::planning, path, processed, total});
    if (before)
      ++left;
    if (after)
      ++right;
  }
  if (verify_objects)
    (void)store.closure({{Kind::revision, std::string(*target)}}, true, options.stop, options.progress);
  Json blockers = Json::array();
  for (const auto &[path, reason, obstruction] : conflicts) {
    throw_if_cancelled(options.stop);
    blockers.push_back(Json{{"path", path}, {"reason", reason}, {"obstruction", obstruction}});
  }
  report_progress(options, {ProgressPhase::planning, {}, processed, total, true});
  auto result = Json{{"version", 1},
                     {"branch", branch},
                     {"source", source_id ? Json(*source_id) : Json{}},
                     {"target", target ? Json(*target) : Json{}},
                     {"staging_snapshot", std::move(staging["snapshot"])},
                     {"policy", "clean-tracked-v1"},
                     {"ready", conflicts.empty()},
                     {"objects_verified", verify_objects},
#ifdef _WIN32
                     {"executable_metadata_supported", false},
#else
                     {"executable_metadata_supported", true},
#endif
                     {"inventory_entries", inventory.size()},
                     {"hashed_bytes", hashed_bytes},
                     {"restore_bytes", restore_bytes},
                     {"counts", counts},
                     {"actions", std::move(actions)},
                     {"create_directories", directories},
                     {"conflicts", std::move(blockers)}};
  if (layout.transition) {
    result["namespace_transition"] = true;
    result["remove_directories"] = layout.remove_directories;
  }
  return result;
}
} // namespace
Json Store::plan_checkout(const fs::path &directory, std::string_view branch, std::string_view target,
                          bool verify_objects, OperationOptions options) const {
  return plan_checkout_impl(*this, directory, branch, nullptr, target, verify_objects, options);
}
Json plan_native_checkout_files(const Store &store, const fs::path &directory, std::string_view branch,
                                const NativeSnapshot &destination, OperationOptions options) {
  return plan_checkout_impl(store, directory, branch, &destination, {}, false, options);
}
namespace {
bool beneath(const std::set<std::string> &prefixes, const std::string &path) {
  if (prefixes.contains(path))
    return true;
  for (auto n = path.find('/'); n != path.npos; n = path.find('/', n + 1))
    if (prefixes.contains(path.substr(0, n)))
      return true;
  return false;
}
bool intersects(const std::set<std::string> &paths, const std::string &path) {
  if (beneath(paths, path))
    return true;
  const auto prefix = path + '/';
  const auto found = paths.lower_bound(prefix);
  return found != paths.end() && found->starts_with(prefix);
}
} // namespace
NativeHardResetPlan prepare_native_hard_reset(const Store &store, const fs::path &directory,
                                              std::string_view branch, std::string_view target,
                                              bool verify_objects, OperationOptions options,
                                              const std::set<std::string> &additional_files) {
  throw_if_cancelled(options.stop);
  validate_id(target);
  require_complete_native_materialization(store);
  const NativeRewriteState state(store, branch, options);
  const auto head = store.native_head();
  require(store.native_target() == branch, "native hard reset requires the selected branch");
  require(head["directory"].is_null() ||
              utf8(fs::canonical(directory)) == head["directory"].get<std::string>(),
          "native HEAD belongs to another workspace");
  const auto original = read_native_snapshot(store, state.head.id, options);
  const auto staged = state.index ? read_native_snapshot(store, state.index->id, options) : NativeSnapshot{};
  const auto destination = read_native_snapshot(store, target, options);
  std::set<std::string> tracked, targets, displaced;
  std::map<std::string, std::pair<std::string, bool>> target_names;
  std::size_t budget = 0;
  for (const auto *snapshot : {&original, &staged})
    for (const auto &[path, entry] : *snapshot) {
      (void)entry;
      tracked.insert(portable_fold(path));
    }
  const auto add_target = [&](const std::string &path, bool is_directory) {
    const auto folded = portable_fold(path);
    const auto [found, added] = target_names.emplace(folded, std::pair{path, is_directory});
    require(added || found->second == std::pair{path, is_directory}, "case-insensitive path collision");
    if (added) {
      const auto cost = path.size() + folded.size() + 64;
      require(cost <= max_object_size - budget, "native checkout inventory exceeds 16 MiB budget");
      budget += cost;
    }
  };
  for (const auto &[path, entry] : destination) {
    (void)entry;
    targets.insert(portable_fold(path));
    add_target(path, false);
    for (auto n = path.find('/'); n != path.npos; n = path.find('/', n + 1))
      add_target(path.substr(0, n), true);
  }
  const Inventory inventory(directory, store, options);
  for (const auto &[path, node] : inventory.nodes())
    if (node.type == Type::directory) {
      const auto folded = portable_fold(path);
      const auto found = target_names.find(folded);
      if (found != target_names.end() && (!found->second.second || found->second.first != path))
        displaced.insert(folded);
    }
  NativeHardResetPlan plan{state.head.id, state.index, {}, {}, {}, {}};
  plan.selection = state.lookup(store.native_workspace_reference("workspace/HEAD"));
  Bytes buffer(1024 * 1024);
  std::map<std::string, std::vector<const NativeSnapshotFile *>> existing;
  for (const auto *snapshot : {&original, &staged, &destination})
    for (const auto &[path, file] : *snapshot)
      existing[portable_fold(path)].push_back(&file);
  Json untracked = Json::array(), working = Json::object();
  std::size_t metadata_budget = 4096;
  for (const auto &[path, node] : inventory.nodes()) {
    throw_if_cancelled(options.stop);
    const auto folded = portable_fold(path);
    const bool obstructing = intersects(targets, folded) || beneath(displaced, folded);
    if (node.type == Type::directory && obstructing) {
      validate_path(path);
      require(plan.directories.size() < 100000, "native hard reset profile path limit exceeded");
      plan.directories.insert(path);
      const auto cost = path.size() + 64;
      require(cost <= max_object_size - metadata_budget, "native hard reset profile exceeds 16 MiB");
      metadata_budget += cost;
    }
    if (node.type != Type::file ||
        (!tracked.contains(folded) && !obstructing && !additional_files.contains(folded)))
      continue;
    validate_path(path);
    const NativeFile file{inventory.base / path_from_utf8(path), path, node.size, node.modified,
                          node.executable};
    const auto content = hash_native_file(file, buffer, options);
    NativeSnapshotFile entry{{}, content, node.executable};
    for (const auto *candidate : existing[folded])
      if (candidate->content == content) {
        entry.manifest = candidate->manifest;
        break;
      }
    const Json profile{{"size", content.size}, {"file_hash", content.hash}, {"executable", node.executable}};
    const auto cost = Json(path).dump().size() + profile.dump().size() + 128;
    require(cost <= max_object_size - metadata_budget, "native hard reset profile exceeds 16 MiB");
    metadata_budget += cost;
    working.get_ref<Json::object_t &>().emplace_back(path, profile);
    plan.files.emplace(path, entry);
    plan.working.push_back(file);
    if (!tracked.contains(folded))
      untracked.push_back(path);
  }
  const auto workspace = plan_checkout_impl(store, directory, branch, &destination, target, false, options,
                                            &plan.files, {}, plan.directories, &inventory);
  if (verify_objects) {
    std::vector<ObjectKey> roots{state.head, {Kind::revision, std::string(target)}};
    if (state.index)
      roots.push_back(*state.index);
    (void)store.closure(roots, true, options.stop, options.progress);
  }
  const Json context{
      {"version", 1},
      {"type", "native-hard-reset-plan"},
      {"directory", utf8(inventory.base)},
      {"store", utf8(store.root())},
      {"branch", branch},
      {"head", state.head.id},
      {"target", target},
      {"index", state.index ? Json(state.index->id) : Json{}},
      {"selection", state.lookup(store.native_workspace_reference("workspace/HEAD")) ? key_json(*state.lookup(store.native_workspace_reference("workspace/HEAD"))) : Json{}},
      {"working", std::move(working)},
      {"directories", plan.directories}};
  plan.report =
      Json{{"version", 1},
           {"operation", "reset"},
           {"mode", "hard"},
           {"branch", branch},
           {"head", state.head.id},
           {"target", target},
           {"index", context["index"]},
           {"plan", hash(bytes("Syrnck hard reset plan v1:" + context.dump()))},
           {"working", context["working"]},
           {"source_directories", plan.directories},
           {"obstructing_untracked", std::move(untracked)},
           {"changed", state.head.id != target || state.index.has_value() || !workspace["actions"].empty()},
           {"ready", workspace["ready"]},
           {"objects_verified", verify_objects},
           {"workspace", workspace}};
  require(plan.report.dump().size() <= max_object_size, "native hard reset report exceeds 16 MiB");
  return plan;
}
Json plan_native_hard_reset(const Store &store, const fs::path &directory, std::string_view branch,
                            std::string_view target, bool verify_objects, OperationOptions options) {
  return prepare_native_hard_reset(store, directory, branch, target, verify_objects, options).report;
}
Json plan_native_checkout_source(const Store &store, const fs::path &directory, std::string_view branch,
                                 std::string_view source, std::string_view target,
                                 const std::set<std::string> &directories, OperationOptions options) {
  const auto files = read_native_snapshot(store, source, options);
  return plan_checkout_impl(store, directory, branch, nullptr, target, false, options, &files,
                            std::string(source), directories);
}

} // namespace syrnck
