#include "native_merge.hpp"
#include <algorithm>
namespace syrnck {
NativeMergePlan prepare_native_merge(const Store &store, const fs::path &directory, std::string_view branch,
                                     std::string_view source, const NativeMergeOptions &settings,
                                     bool verify_objects, OperationOptions options) {
  throw_if_cancelled(options.stop);
  require(!settings.no_fast_forward || !settings.fast_forward_only, "choose either --no-ff or --ff-only");
  validate_ref("heads/" + std::string(branch));
  validate_id(source);
  if (settings.base)
    validate_id(*settings.base);
  const auto selected = store.native_head();
  require(selected["directory"].is_null() ||
              utf8(fs::canonical(directory)) == selected["directory"].get<std::string>(),
          "native HEAD belongs to another workspace");
  const auto refs = store.refs();
  const auto head = refs.find("heads/" + std::string(branch));
  require(head != refs.end() && head->second.kind == Kind::revision, "unknown native branch");
  const auto graph = store.native_ancestry(head->second.id, source, settings.limits, options);
  NativeMergePlan plan{head->second.id, {}, {}, {}};
  const auto &bases = graph["merge_bases"];
  Json blockers = Json::array();
  if (settings.base) {
    require(std::find(bases.begin(), bases.end(), Json(*settings.base)) != bases.end(),
            "native merge base must be a best common ancestor");
    plan.base = settings.base;
  } else if (bases.size() == 1) {
    plan.base = bases[0].get<std::string>();
  } else if (bases.empty() && !settings.allow_unrelated_histories) {
    blockers.push_back(Json{{"reason", "unrelated_histories"}});
  } else if (bases.size() > 1) {
    blockers.push_back(Json{{"reason", "multiple_merge_bases"}});
  }
  const bool up_to_date = graph["target_is_ancestor"].get<bool>();
  const bool forward_possible = graph["source_is_ancestor"].get<bool>();
  const bool forward = !up_to_date && forward_possible && !settings.no_fast_forward;
  if (settings.fast_forward_only && !up_to_date && !forward_possible)
    blockers.push_back(Json{{"reason", "fast_forward_required"}});
  const std::string_view mode = up_to_date ? "up_to_date" : forward ? "fast_forward" : "merge";
  NativeFileMergeResult combined;
  Json workspace;
  if (blockers.empty()) {
    const auto current = read_native_snapshot(store, plan.head, options);
    if (up_to_date) {
      combined.files = current;
    } else {
      const auto incoming = read_native_snapshot(store, source, options);
      const auto original = plan.base ? read_native_snapshot(store, *plan.base, options) : NativeSnapshot{};
      combined = merge_native_files(original, current, incoming, options);
      // Fast-forward adopts the exact source revision, including its manifests.
      if (forward)
        combined.files = incoming;
    }
    if (combined.conflicts.empty())
      workspace = plan_native_checkout_files(store, directory, branch, combined.files, options);
  }
  if (verify_objects)
    (void)store.closure({head->second, {Kind::revision, std::string(source)}}, true, options.stop,
                        options.progress);
  const bool ready = blockers.empty() && combined.conflicts.empty() && workspace["ready"].get<bool>();
  plan.files = std::move(combined.files);
  plan.report = Json{{"version", 1},
                     {"operation", "merge"},
                     {"branch", branch},
                     {"head", plan.head},
                     {"source", source},
                     {"mode", mode},
                     {"base", plan.base ? Json(*plan.base) : Json{}},
                     {"merge_bases", bases},
                     {"visited_revisions", graph["visited_revisions"]},
                     {"parent_links", graph["parent_links"]},
                     {"allow_unrelated_histories", settings.allow_unrelated_histories},
                     {"no_fast_forward", settings.no_fast_forward},
                     {"fast_forward_only", settings.fast_forward_only},
                     {"ready", ready},
                     {"objects_verified", verify_objects},
                     {"result_files", blockers.empty() ? Json(plan.files.size()) : Json{}},
                     {"changes", std::move(combined.changes)},
                     {"conflicts", std::move(combined.conflicts)},
                     {"blockers", std::move(blockers)},
                     {"workspace", std::move(workspace)}};
  require(plan.report.dump().size() <= max_object_size, "native merge report exceeds 16 MiB");
  report_progress(options, {ProgressPhase::planning, {}, combined.processed, combined.paths, true});
  return plan;
}
Json Store::plan_merge(const fs::path &directory, std::string_view branch, std::string_view source,
                       const NativeMergeOptions &settings, bool verify_objects,
                       OperationOptions options) const {
  return prepare_native_merge(*this, directory, branch, source, settings, verify_objects, options).report;
}
} // namespace syrnck
