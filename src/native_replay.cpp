#include "native_replay.hpp"
#include <algorithm>
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
NativeReplayPlan prepare_native_replay(const Store &store, const fs::path &directory, std::string_view branch,
                                       std::string_view source_id, NativeReplayKind kind,
                                       std::optional<std::uint32_t> mainline, bool verify_objects,
                                       OperationOptions options) {
  throw_if_cancelled(options.stop);
  require(kind == NativeReplayKind::cherry_pick || kind == NativeReplayKind::revert,
          "invalid native replay kind");
  validate_ref("heads/" + std::string(branch));
  validate_id(source_id);
  const auto selected_head = store.native_head();
  require(selected_head["directory"].is_null() ||
              utf8(fs::canonical(directory)) == selected_head["directory"].get<std::string>(),
          "native HEAD belongs to another workspace");
  const auto refs = store.refs();
  const auto head = refs.find("heads/" + std::string(branch));
  require(head != refs.end() && head->second.kind == Kind::revision, "unknown native branch");
  const auto revision = store.metadata(Kind::revision, source_id);
  const auto &parents = revision["parents"];
  require(parents.size() <= 1 || mainline.has_value(), "native replay of a merge requires --mainline");
  const auto selected = mainline.value_or(1);
  require((parents.empty() && !mainline) || (selected >= 1 && selected <= parents.size()),
          "native replay mainline is outside the parent list");
  const auto parent = parents.empty() ? std::optional<std::string>{}
                                      : std::optional{parents[selected - 1].get<std::string>()};
  auto original = parent ? read_native_snapshot(store, *parent, options) : NativeSnapshot{};
  auto incoming = read_native_snapshot(store, source_id, options);
  if (kind == NativeReplayKind::revert)
    original.swap(incoming);
  const auto current = read_native_snapshot(store, head->second.id, options);
  NativeReplayPlan plan{head->second.id, {}, {}};
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
      plan.files.emplace_hint(plan.files.end(), path, *chosen);
    }
    if (++completed % 256 == 0)
      report_progress(options, {ProgressPhase::planning, path, completed, paths.size()});
  }
  // Independent additions can make an individually valid pair of snapshots
  // collide in the combined portable namespace. Report these before checkout.
  std::map<std::string, std::string> folded;
  for (const auto &[path, entry] : plan.files) {
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
  Json workspace;
  if (conflicts.empty())
    workspace = plan_native_checkout_files(store, directory, branch, plan.files, options);
  if (verify_objects)
    (void)store.closure({head->second, {Kind::revision, std::string(source_id)}}, true, options.stop,
                        options.progress);
  const bool ready = conflicts.empty() && !changes.empty() && workspace["ready"].get<bool>();
  plan.report = Json{{"version", 1},
                     {"operation", kind == NativeReplayKind::revert ? "revert" : "cherry-pick"},
                     {"branch", branch},
                     {"head", plan.head},
                     {"source", source_id},
                     {"parent", parent ? Json(*parent) : Json{}},
                     {"mainline", parents.empty() ? Json{} : Json(selected)},
                     {"ready", ready},
                     {"empty", changes.empty()},
                     {"objects_verified", verify_objects},
                     {"result_files", plan.files.size()},
                     {"changes", std::move(changes)},
                     {"conflicts", std::move(conflicts)},
                     {"workspace", std::move(workspace)}};
  require(plan.report.dump().size() <= max_object_size, "native replay report exceeds 16 MiB");
  report_progress(options, {ProgressPhase::planning, {}, completed, paths.size(), true});
  return plan;
}
Json Store::plan_replay(const fs::path &directory, std::string_view branch, std::string_view source,
                        NativeReplayKind kind, std::optional<std::uint32_t> mainline, bool verify_objects,
                        OperationOptions options) const {
  return prepare_native_replay(*this, directory, branch, source, kind, mainline, verify_objects, options)
      .report;
}
} // namespace syrnck
