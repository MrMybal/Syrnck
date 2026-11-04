#include "native_workspace.hpp"
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
  require(attributes != INVALID_FILE_ATTRIBUTES, "cannot inspect native checkout entry");
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
  Inventory(const fs::path &directory, const fs::path &store, const OperationOptions &options)
      : base(fs::canonical(directory)) {
    require(base != store && inside(store, base), "native store must be below checkout workspace");
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
  const Node *find(const std::string &path) const {
    const auto found = nodes_.find(path);
    return found == nodes_.end() ? nullptr : &found->second;
  }
  // Report namespace obstructions before opening anything. Links/reparse
  // points are opaque, including inside ignored directories.
  bool inspect(const std::string &path, Conflicts &conflicts, bool leaf) const {
    const auto folded = folded_.find(portable_fold(path));
    if (folded == folded_.end())
      return true;
    if (folded->second.size() != 1) {
      for (const auto *node : folded->second)
        conflicts.emplace(path, "portable_collision", node->first);
      return false;
    }
    const auto &[actual, node] = *folded->second.front();
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
  bool parents(const std::string &path, Conflicts &conflicts) const {
    bool valid = true;
    for (auto n = path.find('/'); n != path.npos; n = path.find('/', n + 1))
      if (!inspect(path.substr(0, n), conflicts, false)) {
        valid = false;
        break;
      }
    return valid;
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
void verify_native_checkout_namespace(const Store &store, const fs::path &directory,
                                      const std::vector<std::string> &target,
                                      const std::vector<std::string> &removed, OperationOptions options,
                                      bool final) {
  const Inventory inventory(directory, store.root(), options);
  Conflicts conflicts;
  for (const auto &path : target) {
    throw_if_cancelled(options.stop);
    require(inventory.parents(path, conflicts) && inventory.inspect(path, conflicts, true) &&
                (!final || inventory.find(path)),
            "native checkout path became obstructed");
  }
  for (const auto &path : removed) {
    throw_if_cancelled(options.stop);
    require(inventory.parents(path, conflicts) && inventory.inspect(path, conflicts, true) &&
                (!final || !inventory.find(path)),
            "native checkout path became obstructed");
  }
}
Json Store::plan_checkout(const fs::path &directory, std::string_view branch, std::string_view target,
                          bool verify_objects, OperationOptions options) const {
  throw_if_cancelled(options.stop);
  auto staging = staged_status(branch, options);
  const auto source_id = staging["head"].is_null() ? std::optional<std::string>{}
                                                   : std::optional{staging["head"].get<std::string>()};
  const auto source = source_id ? read_native_snapshot(*this, *source_id, options) : NativeSnapshot{};
  const auto other =
      source_id && *source_id == target ? NativeSnapshot{} : read_native_snapshot(*this, target, options);
  const auto &destination = source_id && *source_id == target ? source : other;
  const Inventory inventory(directory, root(), options);
  Conflicts conflicts;
  if (refs().contains("checkouts/active"))
    conflicts.emplace("", "checkout_pending", "");
  if (!staging["snapshot"].is_null())
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
    const auto content = hash_native_file(
        {inventory.base / path_from_utf8(path), path, node->size, node->modified, node->executable}, buffer,
        options);
    require(content.size <= UINT64_MAX - hashed_bytes, "native comparison byte count overflow");
    hashed_bytes += content.size;
    if (content != entry.content)
      conflicts.emplace(path, "tracked_modified", path);
    if (!executable_equal(node->executable, entry.executable))
      conflicts.emplace(path, "tracked_mode_changed", path);
  }
  std::set<std::string> directories;
  for (const auto &[path, entry] : destination) {
    (void)entry;
    throw_if_cancelled(options.stop);
    if (!inventory.parents(path, conflicts) || !inventory.inspect(path, conflicts, true))
      continue;
    if (inventory.find(path) && !source.contains(path))
      conflicts.emplace(path, "untracked_collision", path);
    for (auto n = path.find('/'); n != path.npos; n = path.find('/', n + 1)) {
      auto parent = path.substr(0, n);
      if (!inventory.find(parent))
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
    (void)closure({{Kind::revision, std::string(target)}}, true, options.stop, options.progress);
  Json blockers = Json::array();
  for (const auto &[path, reason, obstruction] : conflicts) {
    throw_if_cancelled(options.stop);
    blockers.push_back(Json{{"path", path}, {"reason", reason}, {"obstruction", obstruction}});
  }
  report_progress(options, {ProgressPhase::planning, {}, processed, total, true});
  return Json{{"version", 1},
              {"branch", branch},
              {"source", source_id ? Json(*source_id) : Json{}},
              {"target", target},
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
}
} // namespace syrnck
