#include <syrnck/watcher.hpp>
#include <syrnck/working_index.hpp>
#include "working_file_stamp.hpp"
#include "native_inventory.hpp"
namespace syrnck {
namespace {
bool covered(const std::set<std::string> &scopes, std::string_view path) {
  for (;;) {
    if (scopes.contains(std::string(path))) return true;
    const auto slash = path.rfind('/');
    if (slash == path.npos) return false;
    path = path.substr(0, slash);
  }
}
template<class Map> std::size_t erase_scope(Map &files, std::string_view scope) {
  std::size_t budget = 0;
  if (const auto exact = files.find(std::string(scope)); exact != files.end()) {
    budget += exact->first.size() + 256;
    files.erase(exact);
  }
  const auto prefix = std::string(scope) + '/';
  auto row = files.lower_bound(prefix);
  while (row != files.end() && row->first.starts_with(prefix)) {
    budget += row->first.size() + 256;
    row = files.erase(row);
  }
  return budget;
}
} // namespace
struct WorkspaceObservation::Impl {
  fs::path directory, storage;
  NativeSnapshot current;
  bool ready = false;
  std::string materialization_epoch;
  Impl(const fs::path &root, const fs::path &store)
      : directory(fs::canonical(root)), storage(fs::canonical(store)) {}

  Json observe(const Store &store, const std::optional<std::string> &revision,
               const WorkspaceChanges &changes, OperationOptions options) {
    throw_if_cancelled(options.stop);
    require(store.root() == storage, "workspace observation belongs to another store");
    const auto materialization = read_native_materialization(store).policy;
    const auto epoch = materialization.identity();
    bool full = !ready || changes.full_scan || changes.overflow || epoch != materialization_epoch;
    // Any failure invalidates continuity, including a cancelled query. Only a
    // completely successful observation can become the next incremental base.
    ready = false;
    std::map<std::string, NativeFile> selected;
    std::set<std::string> scopes;
    if (!full && !changes.polling) {
      require(changes.paths.size() <= 65536, "watch path limit must be in 1..65536");
      std::size_t hint_budget = 0, selected_budget = 0;
      std::set<std::string> targets;
      for (const auto &path : changes.paths) {
        throw_if_cancelled(options.stop);
        require(path.size() + 64 <= 1024 * 1024 - hint_budget, "workspace observation paths exceed 1 MiB");
        hint_budget += path.size() + 64;
        // Nested rule changes reselect their complete parent subtree. Root
        // rules affect the entire workspace and therefore still require audit.
        auto target = path;
        if (path_from_utf8(path).filename() == ".syrnckignore") {
          const auto slash = path.rfind('/');
          if (slash == path.npos) { full = true; break; }
          target = path.substr(0, slash);
        }
        targets.insert(std::move(target));
      }
      try {
        for (const auto &target : targets) {
          if (full) break;
          throw_if_cancelled(options.stop);
          if (covered(scopes, target)) continue;
          std::map<std::string, NativeFile> local;
          const auto scope = visit_native_scope(directory, store, target, [&](const NativeFile &file) {
            local.emplace(file.relative, file);
          }, options);
          // Resolution can broaden to an absent/ignored/file ancestor. Drop
          // earlier overlapping selections before publishing this whole scope.
          const auto prefix = scope + '/';
          selected_budget -= erase_scope(selected, scope);
          for (auto &[path, file] : local) {
            require(path.size() + 256 <= max_object_size - selected_budget, "native workspace scope exceeds 16 MiB");
            selected_budget += path.size() + 256;
            selected.emplace(path, std::move(file));
          }
          for (auto row = scopes.lower_bound(prefix); row != scopes.end() && row->starts_with(prefix);)
            row = scopes.erase(row);
          scopes.insert(scope);
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
      for (const auto &scope : scopes) erase_scope(next, scope);
      NativeInventoryPaths names;
      for (const auto &[path, file] : next) {
        (void)file;
        throw_if_cancelled(options.stop);
        names.add(path);
      }
      for (const auto &[path, file] : selected) {
        (void)file;
        throw_if_cancelled(options.stop);
        names.add(path);
      }
      names.finish(options.stop);
      Bytes buffer(1024 * 1024);
      std::uint64_t hashed = 0;
      for (const auto &[path, file] : selected) {
        const auto before = working_file_stamp(file);
        // A notification always forces content reads, even with an unchanged
        // stamp or a filesystem on which stat-based reuse is unsupported.
        auto content = hash_native_file(file, buffer, options);
        const auto after = working_file_stamp(file);
        if (before)
          require(after && before->id == after->id, "file changed during indexed native scan; retry operation");
        require(content.size <= UINT64_MAX - hashed, "native comparison byte count overflow");
        hashed += content.size;
        next.emplace(path, NativeSnapshotFile{{}, std::move(content), file.executable});
      }
      const auto baseline = revision ? read_native_materialized_snapshot(store, *revision, materialization, options) : NativeSnapshot{};
      auto comparison = compare_native_workspace_maps(baseline, next, options);
      report = Json{{"version", 1}, {"base", revision ? Json(*revision) : Json{}},
                    {"clean", comparison["changes"].empty()}, {"tracked_files", baseline.size()},
                    {"workspace_files", next.size()}, {"hashed_bytes", hashed},
                    {"counts", std::move(comparison["counts"])}, {"changes", std::move(comparison["changes"])},
                    {"observation", Json{{"version", 1}, {"mode", "targeted"}, {"full_scan", false},
                                          {"inspected_files", selected.size()},
                                          {"retained_files", next.size() - selected.size()}, {"scanned_scopes", scopes}}}};
      current = std::move(next);
    }
    throw_if_cancelled(options.stop);
    materialization_epoch = epoch;
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
void WorkspaceObservation::adopt_capture(const Store &store, const fs::path &directory, std::string_view revision) {
  auto &state = *impl_;
  state.ready = false;
  require(store.root() == state.storage && directory == state.directory,
          "workspace observation belongs to another store");
  const auto materialization = read_native_materialization(store).policy;
  auto captured = read_native_materialized_snapshot(store, revision, materialization, {});
  for (auto &[path, file] : captured) { (void)path; file.manifest.clear(); }
  state.current = std::move(captured);
  state.materialization_epoch = materialization.identity();
  state.ready = true;
}
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
