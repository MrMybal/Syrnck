#include "git_internal.hpp"
#include <syrnck/git.hpp>
#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <sstream>
namespace syrnck::git {
namespace {
constexpr std::size_t object_limit = 524288, manifest_limit = 65536;
void complete_history(const fs::path &directory, OperationOptions operation) {
  for (const auto name : {"GIT_NAMESPACE", "GIT_INDEX_FILE", "GIT_SHALLOW_FILE", "GIT_DIR", "GIT_WORK_TREE",
                          "GIT_COMMON_DIR", "GIT_OBJECT_DIRECTORY"}) {
    auto value = std::getenv(name);
    require(!value || !*value, "Git retention requires the standard repository environment");
  }
  auto shallow =
      detail::local_git(directory, {"rev-parse", "--is-shallow-repository"}, {}, 1024, operation.stop);
  require(shallow.status == 0 && detail::trim_line(shallow.out) == "false",
          "Git retention requires complete Git history");
  auto grafts = common_store(directory).parent_path() / "info/grafts";
  auto status = fs::symlink_status(grafts);
  require(!fs::is_symlink(status) && (status.type() == fs::file_type::not_found ||
                                      (fs::is_regular_file(status) && fs::file_size(grafts) == 0)),
          "Git retention does not support Git graft history");
}
} // namespace
Json RetentionInventory::json() const {
  auto assets = Json::array();
  for (const auto &pointer : manifests)
    assets.push_back(Json{{"manifest", pointer.manifest}, {"size", pointer.size}});
  return Json{{"version", 1},       {"repository", repository},  {"fingerprint", fingerprint},
              {"objects", objects}, {"commits", commits.size()}, {"manifests", std::move(assets)}};
}
RetentionInventory retention_inventory(const fs::path &directory, OperationOptions operation) {
  throw_if_cancelled(operation.stop);
  complete_history(directory, operation);
  const auto common_path = fs::canonical(common_store(directory).parent_path());
  auto common = utf8(common_path);
#ifdef _WIN32
  common = portable_fold(common);
#endif
  RetentionInventory result;
  result.repository = hash(bytes("syrnck-git-repository-v1\n" + common));
  report_progress(operation, {ProgressPhase::scanning, "git-retention"});
  const auto worktrees = detail::local_git(directory, {"worktree", "list", "--porcelain", "-z"}, {},
                                           1024 * 1024, operation.stop);
  require(worktrees.status == 0, "cannot enumerate Git retention worktrees");
  std::set<fs::path> directories;
  for (std::size_t at = 0; at < worktrees.out.size();) {
    const auto end = worktrees.out.find('\0', at);
    require(end != worktrees.out.npos, "invalid Git worktree response");
    const auto item = std::string_view(worktrees.out).substr(at, end - at);
    if (item.starts_with("worktree ")) {
      const auto path = path_from_utf8(item.substr(9));
      require(path.is_absolute() && directories.insert(path).second && directories.size() <= 4096,
              "invalid Git retention worktree list");
    }
    at = end + 1;
  }
  require(!directories.empty(), "invalid Git retention worktree list");
  std::set<std::string> objects;
  std::string line;
  for (const auto &worktree : directories) {
    throw_if_cancelled(operation.stop);
    require(fs::equivalent(common_store(worktree).parent_path(), common_path),
            "Git retention worktree belongs to another repository");
    auto listed = detail::local_git(worktree,
                                    {"rev-list", "--single-worktree", "--objects", "--all", "--reflog",
                                     "--indexed-objects", "--no-object-names", "--filter=blob:limit=1025"},
                                    {}, 64 * 1024 * 1024, operation.stop);
    require(listed.status == 0, "cannot enumerate Git retention objects");
    require(listed.err.empty(),
            "Git retention traversal emitted diagnostics; repair Git metadata or disable tracing");
    std::istringstream lines(listed.out);
    while (std::getline(lines, line)) {
      throw_if_cancelled(operation.stop);
      detail::validate_oid(line);
      objects.insert(line);
      require(objects.size() <= object_limit, "Git retention object limit exceeded");
    }
    require(lines.eof(), "invalid Git retention object response");
  }
  std::string identity = "syrnck-git-retention-v1\n" + result.repository + '\n';
  std::map<std::string, std::uint64_t> manifests;
  auto visit = [&](const std::string &, std::string_view raw) {
    if (auto pointer = parse_pointer(raw)) {
      auto [at, inserted] = manifests.emplace(pointer->manifest, pointer->size);
      require(inserted || at->second == pointer->size, "inconsistent Git retention pointer size");
      require(manifests.size() <= manifest_limit, "Git retention manifest limit exceeded");
    }
  };
  std::size_t processed = 0;
  for (auto cursor = objects.begin(); cursor != objects.end();) {
    std::vector<detail::TreeEntry> blobs;
    std::vector<std::string> batch;
    std::string input;
    while (cursor != objects.end() && batch.size() < 512) {
      batch.push_back(*cursor);
      input += *cursor + '\n';
      identity += *cursor + '\n';
      ++cursor;
    }
    auto checked =
        detail::local_git(directory, {"cat-file", "--batch-check"}, input, 64 * 1024, operation.stop);
    require(checked.status == 0, "cannot inspect Git retention objects");
    std::istringstream info(checked.out);
    for (const auto &expected : batch) {
      throw_if_cancelled(operation.stop);
      require(bool(std::getline(info, line)), "invalid Git retention metadata response");
      std::istringstream fields(line);
      std::string oid, type, size, extra;
      require(bool(fields >> oid >> type >> size) && !(fields >> extra) && oid == expected,
              "invalid Git retention metadata response");
      std::uint64_t length = 0;
      auto parsed = std::from_chars(size.data(), size.data() + size.size(), length);
      require(parsed.ec == std::errc{} && parsed.ptr == size.data() + size.size(),
              "invalid Git retention metadata response");
      require(type == "commit" || type == "tree" || type == "blob" || type == "tag",
              "invalid Git retention metadata response");
      if (type == "commit")
        result.commits.insert(oid);
      if (type == "blob" && length <= 1024)
        blobs.push_back({oid, oid, "100644", "blob", length});
    }
    require(!std::getline(info, line) && info.eof(), "trailing Git retention metadata response");
    detail::visit_small_blobs(directory, blobs, "git-retention", visit, {operation.stop, {}});
    processed += batch.size();
    report_progress(operation, {ProgressPhase::scanning, "git-retention", processed, objects.size()});
  }
  for (const auto &[id, size] : manifests)
    result.manifests.push_back({id, size});
  result.objects = objects.size();
  result.fingerprint = hash(bytes(identity));
  report_progress(operation, {ProgressPhase::scanning, "git-retention", processed, objects.size(), true});
  return result;
}
Json reconcile_retention(const Store &store, const fs::path &directory, bool apply,
                         OperationOptions operation) {
  const auto inventory = retention_inventory(directory, operation);
  const auto prefix = "git-live/" + inventory.repository + '/';
  const auto refs = store.refs();
  std::vector<RefUpdate> updates;
  std::vector<ObjectKey> roots;
  std::set<std::string> live;
  auto additions = Json::object(), unobserved = Json::object();
  for (const auto &pointer : inventory.manifests) {
    live.insert(pointer.manifest);
    ObjectKey key{Kind::manifest, pointer.manifest};
    roots.push_back(key);
    const auto name = prefix + pointer.manifest;
    const auto at = refs.find(name);
    require(at == refs.end() || at->second == key, "Git retention pin points to another object");
    if (at == refs.end()) {
      updates.push_back({name, {}, key});
      additions[name] = key_json(key);
    }
  }
  for (const auto &[name, key] : refs) {
    throw_if_cancelled(operation.stop);
    if (name.starts_with(prefix)) {
      const auto suffix = name.substr(prefix.size());
      validate_id(suffix);
      require(key == ObjectKey{Kind::manifest, suffix}, "Git retention pin points to another object");
      if (!live.contains(suffix))
        unobserved[name] = Json{{"key", key_json(key)}, {"scope", "repository"}};
    } else if (name.starts_with("git/") && key.kind == Kind::manifest && name == "git/" + key.id &&
               !live.contains(key.id))
      unobserved[name] = Json{{"key", key_json(key)}, {"scope", "legacy"}};
    else if (name.starts_with("git-assets/") && key.kind == Kind::revision &&
             !inventory.commits.contains(name.substr(11)))
      unobserved[name] = Json{{"key", key_json(key)}, {"scope", "legacy"}};
  }
  auto result = inventory.json();
  result["add"] = std::move(additions);
  result["unobserved_pins"] = std::move(unobserved);
  result["ready"] = true;
  result["verification_error"] = nullptr;
  result["applied"] = false;
  result["added"] = 0;
  try {
    for (const auto &pointer : inventory.manifests) {
      throw_if_cancelled(operation.stop);
      require(store.metadata(Kind::manifest, pointer.manifest)["size"].get<std::uint64_t>() == pointer.size,
              "pointer size mismatch");
    }
    (void)store.closure(roots, true, operation.stop, operation.progress);
  } catch (const Cancelled &) {
    throw;
  } catch (const Error &error) {
    result["ready"] = false;
    result["verification_error"] = error.what();
  }
  if (!apply || !result["ready"].get<bool>())
    return result;
  report_progress(operation, {ProgressPhase::planning, "git-retention", inventory.manifests.size(),
                              inventory.manifests.size(), true});
  require(retention_inventory(directory, operation).fingerprint == inventory.fingerprint,
          "Git retention inventory changed; retry reconciliation");
  require(store.compare_update_refs(updates, operation), "Git retention pins changed; retry reconciliation");
  // Publication has completed. No further callbacks or cancellation checks.
  result["applied"] = true;
  result["added"] = updates.size();
  return result;
}
} // namespace syrnck::git
