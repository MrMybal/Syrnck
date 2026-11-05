#include "native_merge.hpp"
namespace syrnck {
namespace {
const NativeSnapshotFile *lookup(const NativeSnapshot &files, const std::string &path) {
  const auto found = files.find(path);
  return found == files.end() ? nullptr : &found->second;
}
bool equal(const NativeSnapshotFile *a, const NativeSnapshotFile *b) {
  return (!a && !b) || (a && b && a->content == b->content && a->executable == b->executable);
}
Json summary(const NativeSnapshotFile *file) {
  return file ? file->json() : Json{};
}
class ReportBudget {
  std::size_t bytes_ = 4096;

public:
  void append(Json &rows, Json row) {
    const auto size = row.dump().size() + 1;
    require(size <= max_object_size - bytes_, "native replay report exceeds 16 MiB");
    bytes_ += size;
    rows.push_back(std::move(row));
  }
};
void namespace_conflicts(const NativeSnapshot &files, Json &conflicts, ReportBudget &budget,
                         const OperationOptions &options) {
  std::map<std::string, std::string> folded;
  for (const auto &[path, entry] : files) {
    (void)entry;
    throw_if_cancelled(options.stop);
    const auto [found, added] = folded.emplace(portable_fold(path), path);
    if (!added)
      budget.append(conflicts, Json{{"path", path}, {"reason", "case_collision"}, {"other", found->second}});
  }
  for (const auto &[fold, path] : folded) {
    throw_if_cancelled(options.stop);
    for (auto n = fold.find('/'); n != fold.npos; n = fold.find('/', n + 1))
      if (const auto found = folded.find(fold.substr(0, n)); found != folded.end())
        budget.append(conflicts,
                      Json{{"path", path}, {"reason", "file_directory_collision"}, {"other", found->second}});
  }
}
} // namespace
void require_native_resolution_context(const NativeResolutionSet &resolutions, std::string_view operation,
                                       std::string_view head, std::string_view source,
                                       const std::optional<std::string> &base,
                                       const std::optional<std::string> &parent) {
  resolutions.validate();
  require(resolutions.operation == operation && resolutions.head == head && resolutions.source == source &&
              resolutions.base == base && resolutions.parent == parent,
          "native resolution context changed; refresh the plan");
}
NativeFileMergeResult merge_native_files(const Store &store, const NativeSnapshot &original,
                                         const NativeSnapshot &current, const NativeSnapshot &incoming,
                                         OperationOptions options, const NativeResolutionSet *resolutions,
                                         const TextDiffOptions *text_limits) {
  NativeFileMergeResult result;
  std::set<std::string> paths;
  for (const auto *files : std::array<const NativeSnapshot *, 3>{&original, &incoming, &current})
    for (const auto &[path, entry] : *files) {
      (void)entry;
      throw_if_cancelled(options.stop);
      paths.insert(path);
    }
  Json changes = Json::array(), conflicts = Json::array();
  ReportBudget budget;
  std::uint64_t completed = 0;
  for (const auto &path : paths) {
    throw_if_cancelled(options.stop);
    const auto *base = lookup(original, path), *ours = lookup(current, path),
               *theirs = lookup(incoming, path);
    const NativeSnapshotFile *chosen = ours;
    std::optional<NativeSnapshotFile> merged;
    std::string_view reason;
    if (equal(ours, theirs) || equal(base, theirs)) {
      // Keep our manifest when logical content/mode already has the desired value.
    } else if (equal(base, ours)) {
      chosen = theirs;
    } else if (!base) {
      reason = "add_add";
    } else if (!ours || !theirs) {
      reason = ours ? "modify_delete" : "delete_modify";
    } else {
      merged = *ours;
      if (ours->content == theirs->content || theirs->content == base->content) {
        // Preserve our representation when only the other mode differs.
      } else if (ours->content == base->content) {
        merged->manifest = theirs->manifest;
        merged->content = theirs->content;
      } else {
        reason = "content";
      }
      merged->executable = ours->executable == base->executable ? theirs->executable : ours->executable;
      chosen = &*merged;
    }
    if (!reason.empty()) {
      budget.append(conflicts, Json{{"path", path},
                                    {"reason", reason},
                                    {"base", summary(base)},
                                    {"current", summary(ours)},
                                    {"incoming", summary(theirs)}});
      chosen = ours;
    }
    if (chosen)
      result.files.emplace_hint(result.files.end(), path, *chosen);
    if (++completed % 256 == 0)
      report_progress(options, {ProgressPhase::planning, path, completed, paths.size()});
  }
  namespace_conflicts(result.files, conflicts, budget, options);
  Json final_conflicts = Json::array();
  ReportBudget final_budget;
  if (resolutions && !resolutions->files.empty()) {
    std::set<std::string> allowed;
    for (const auto &row : conflicts) {
      allowed.insert(row["path"].get<std::string>());
      if (row.contains("other"))
        allowed.insert(row["other"].get<std::string>());
    }
    std::set<ObjectKey> roots;
    for (const auto &[path, resolution] : resolutions->files) {
      throw_if_cancelled(options.stop);
      require(allowed.contains(path), "native resolution path has no conflict");
      const NativeSnapshotFile *chosen = nullptr;
      std::optional<NativeSnapshotFile> custom;
      switch (resolution.choice) {
      case NativeResolutionChoice::current:
        chosen = lookup(current, path);
        break;
      case NativeResolutionChoice::incoming:
        chosen = lookup(incoming, path);
        break;
      case NativeResolutionChoice::base:
        chosen = lookup(original, path);
        break;
      case NativeResolutionChoice::remove:
        break;
      case NativeResolutionChoice::file: {
        const auto value = store.metadata(Kind::manifest, resolution.manifest);
        custom =
            NativeSnapshotFile{resolution.manifest,
                               {value["size"].get<std::uint64_t>(), value["file_hash"].get<std::string>()},
                               resolution.executable};
        chosen = &*custom;
        roots.insert({Kind::manifest, resolution.manifest});
        break;
      }
      default:
        throw Error("invalid native resolution choice");
      }
      if (chosen)
        result.files.insert_or_assign(path, *chosen);
      else
        result.files.erase(path);
      final_budget.append(result.resolved,
                          Json{{"path", path},
                               {"choice", resolution.choice == NativeResolutionChoice::current    ? "current"
                                          : resolution.choice == NativeResolutionChoice::incoming ? "incoming"
                                          : resolution.choice == NativeResolutionChoice::base     ? "base"
                                          : resolution.choice == NativeResolutionChoice::remove   ? "remove"
                                                                                                  : "file"},
                               {"target", summary(chosen)}});
    }
    for (const auto &row : conflicts)
      if (!row.contains("other") && !resolutions->files.contains(row["path"].get<std::string>()))
        final_budget.append(final_conflicts, row);
    namespace_conflicts(result.files, final_conflicts, final_budget, options);
    result.extra_roots.assign(roots.begin(), roots.end());
  } else {
    for (const auto &row : conflicts)
      final_budget.append(final_conflicts, row);
  }
  if (text_limits) {
    NativeTextMergeBatch batch(store, *text_limits);
    Json remaining = Json::array();
    for (const auto &row : final_conflicts) {
      throw_if_cancelled(options.stop);
      bool resolved = false;
      if (row["reason"] == "content") {
        const auto path = row["path"].get<std::string>();
        const auto merged = batch.merge(path, *lookup(original, path), *lookup(current, path),
                                        *lookup(incoming, path), options);
        if (merged) {
          result.files.insert_or_assign(path, *merged);
          resolved = true;
        }
      }
      if (!resolved)
        remaining.push_back(row);
    }
    final_conflicts = std::move(remaining);
    result.text_merge = batch.report();
    result.generated = std::move(batch.generated);
  }
  std::size_t snapshot_bytes = 2;
  for (const auto &[path, entry] : result.files) {
    throw_if_cancelled(options.stop);
    const auto size = Json(path).dump().size() + 2 +
                      Json{{"manifest", entry.manifest}, {"executable", entry.executable}}.dump().size();
    require(size <= max_object_size - 4096 - snapshot_bytes, "native replay snapshot exceeds 16 MiB");
    snapshot_bytes += size;
  }
  for (const auto &path : paths) {
    throw_if_cancelled(options.stop);
    const auto *ours = lookup(current, path), *chosen = lookup(result.files, path);
    if (!equal(ours, chosen))
      final_budget.append(changes, Json{{"path", path},
                                        {"action", !ours     ? "create"
                                                   : !chosen ? "remove"
                                                             : "replace"},
                                        {"current", summary(ours)},
                                        {"target", summary(chosen)}});
  }
  result.changes = std::move(changes);
  result.conflicts = std::move(final_conflicts);
  result.processed = completed;
  result.paths = paths.size();
  return result;
}
} // namespace syrnck
