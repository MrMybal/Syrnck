#include <syrnck/watcher.hpp>
#include <syrnck/working_index.hpp>
#include "working_file_stamp.hpp"
namespace syrnck {
struct WorkspaceObservation::Impl {
  fs::path directory, storage;
  NativeSnapshot current;
  bool ready = false;
  Impl(const fs::path &root, const fs::path &store)
      : directory(fs::canonical(root)), storage(fs::canonical(store)) {}

  Json observe(const Store &store, const std::optional<std::string> &revision,
               const WorkspaceChanges &changes, OperationOptions options) {
    throw_if_cancelled(options.stop);
    require(store.root() == storage, "workspace observation belongs to another store");
    bool full = !ready || changes.full_scan || changes.overflow;
    // Any failure invalidates continuity, including a cancelled query. Only a
    // completely successful observation can become the next incremental base.
    ready = false;
    std::vector<NativeFile> selected;
    if (!full && !changes.polling) {
      std::set<std::string> unique;
      try {
        for (const auto &path : changes.paths) {
          throw_if_cancelled(options.stop);
          // Namespace/ignore changes can affect siblings or entire subtrees.
          // Initially only existing regular-file edits use the fast path.
          if (!current.contains(path) || path_from_utf8(path).filename() == ".syrnckignore") {
            full = true; break;
          }
          if (!unique.insert(path).second) continue;
          auto file = inspect_native_file(directory, storage, path, options);
          if (!file) { full = true; break; }
          selected.push_back(std::move(*file));
        }
      } catch (const Cancelled &) { throw; }
        catch (const Error &) { full = true; }
        catch (const fs::filesystem_error &) { full = true; }
    }
    Json report;
    if (full || changes.polling) {
      if (full) clear_native_working_index(store, options);
      NativeSnapshot observed;
      report = indexed_native_observation(store, directory, revision, observed, options);
      report["observation"] = Json{{"version", 1}, {"mode", full ? "full" : "indexed"},
                                    {"full_scan", full}, {"inspected_files", observed.size()},
                                    {"retained_files", 0}};
      current = std::move(observed);
    } else {
      auto next = current;
      Bytes buffer(1024 * 1024);
      std::uint64_t hashed = 0;
      for (const auto &file : selected) {
        const auto before = working_file_stamp(file);
        // A notification always forces content reads, even with an unchanged
        // stamp or a filesystem on which stat-based reuse is unsupported.
        auto content = hash_native_file(file, buffer, options);
        const auto after = working_file_stamp(file);
        if (before)
          require(after && before->id == after->id, "file changed during indexed native scan; retry operation");
        require(content.size <= UINT64_MAX - hashed, "native comparison byte count overflow");
        hashed += content.size;
        next.at(file.relative) = NativeSnapshotFile{{}, std::move(content), file.executable};
      }
      const auto baseline = revision ? read_native_snapshot(store, *revision, options) : NativeSnapshot{};
      auto comparison = compare_native_workspace_maps(baseline, next, options);
      report = Json{{"version", 1}, {"base", revision ? Json(*revision) : Json{}},
                    {"clean", comparison["changes"].empty()}, {"tracked_files", baseline.size()},
                    {"workspace_files", next.size()}, {"hashed_bytes", hashed},
                    {"counts", std::move(comparison["counts"])}, {"changes", std::move(comparison["changes"])},
                    {"observation", Json{{"version", 1}, {"mode", "targeted"}, {"full_scan", false},
                                          {"inspected_files", selected.size()},
                                          {"retained_files", next.size() - selected.size()}}}};
      current = std::move(next);
    }
    throw_if_cancelled(options.stop);
    ready = true;
    return report;
  }
};
WorkspaceObservation::WorkspaceObservation(const fs::path &directory, const fs::path &store)
    : impl_(std::make_unique<Impl>(directory, store)) {}
WorkspaceObservation::~WorkspaceObservation() = default;
WorkspaceObservation::WorkspaceObservation(WorkspaceObservation &&) noexcept = default;
WorkspaceObservation &WorkspaceObservation::operator=(WorkspaceObservation &&) noexcept = default;
void WorkspaceObservation::invalidate() { impl_->ready = false; }
Json WorkspaceObservation::status(const Store &store, const std::optional<std::string> &revision,
                                  const WorkspaceChanges &changes, OperationOptions options) {
  try { return impl_->observe(store, revision, changes, options); }
  catch (...) { invalidate(); throw; }
}
Json WorkspaceObservation::workspace_status(const Store &store, std::string_view branch,
                                            const WorkspaceChanges &changes, OperationOptions options) {
  // Staging failures must invalidate the session just like working-file errors.
  try {
    auto staged = store.staged_status(branch, options);
    const auto &effective = staged["snapshot"].is_null() ? staged["head"] : staged["snapshot"];
    auto unstaged = status(store, effective.is_null() ? std::optional<std::string>{}
                                                    : std::optional{effective.get<std::string>()}, changes, options);
    const auto clean = !staged["stale"].get<bool>() && staged["clean"].get<bool>() && unstaged["clean"].get<bool>();
    return Json{{"version", 1}, {"branch", branch}, {"clean", clean},
                {"staged", std::move(staged)}, {"unstaged", std::move(unstaged)}};
  } catch (...) { invalidate(); throw; }
}
} // namespace syrnck
