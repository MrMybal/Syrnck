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
} // namespace
NativeFileMergeResult merge_native_files(const NativeSnapshot &original, const NativeSnapshot &current,
                                         const NativeSnapshot &incoming, OperationOptions options) {
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
  std::size_t snapshot_bytes = 2;
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
    } else if (!equal(ours, chosen)) {
      budget.append(changes, Json{{"path", path},
                                  {"action", !ours     ? "create"
                                             : !chosen ? "remove"
                                                       : "replace"},
                                  {"current", summary(ours)},
                                  {"target", summary(chosen)}});
    }
    if (chosen) {
      const auto size =
          Json(path).dump().size() + 2 +
          Json{{"manifest", chosen->manifest}, {"executable", chosen->executable}}.dump().size();
      require(size <= max_object_size - 4096 - snapshot_bytes, "native replay snapshot exceeds 16 MiB");
      snapshot_bytes += size;
      result.files.emplace_hint(result.files.end(), path, *chosen);
    }
    if (++completed % 256 == 0)
      report_progress(options, {ProgressPhase::planning, path, completed, paths.size()});
  }
  // Independent additions can make an individually valid pair of snapshots
  // collide in the combined portable namespace. Report these before checkout.
  std::map<std::string, std::string> folded;
  for (const auto &[path, entry] : result.files) {
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
  result.changes = std::move(changes);
  result.conflicts = std::move(conflicts);
  result.processed = completed;
  result.paths = paths.size();
  return result;
}
} // namespace syrnck
