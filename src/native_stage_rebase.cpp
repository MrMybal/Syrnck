#include "native_stage_rebase.hpp"
namespace syrnck {
namespace {
// Keep an explicitly prepared representation when upstream still uses the
// baseline's representation/content. Independent executable changes combine.
void preserve_representations(const NativeSnapshot &base, const NativeSnapshot &current,
                              const NativeSnapshot &incoming, NativeSnapshot &result,
                              OperationOptions options) {
  for (const auto &[path, file] : incoming) {
    throw_if_cancelled(options.stop);
    const auto old = base.find(path), upstream = current.find(path);
    auto chosen = result.find(path);
    if (old != base.end() && upstream != current.end() && chosen != result.end() &&
        file.content == old->second.content && file.manifest != old->second.manifest &&
        upstream->second.content == old->second.content &&
        upstream->second.manifest == old->second.manifest && chosen->second.content == file.content)
      chosen->second.manifest = file.manifest;
  }
}
Json snapshot(const NativeRewriteState &state, const NativeSnapshot &files, OperationOptions options) {
  Json entries = Json::object();
  auto &object = entries.get_ref<Json::object_t &>();
  object.reserve(files.size());
  for (const auto &[path, file] : files) {
    throw_if_cancelled(options.stop);
    object.emplace_back(path, Json{{"manifest", file.manifest}, {"executable", file.executable}});
  }
  return Json{{"version", 1},
              {"parents", Json::array({state.head.id})},
              {"message", "Syrnck staging v1: " + state.branch},
              {"timestamp", 0},
              {"files", std::move(entries)}};
}
} // namespace
NativeStageRebasePlan prepare_native_stage_rebase(const Store &store, const NativeRewriteState &state,
                                                  bool verify_objects, OperationOptions options) {
  throw_if_cancelled(options.stop);
  require(state.index.has_value(), "no native staging snapshot");
  const bool changed = state.index_base != state.head.id;
  const auto archive = "archive/rebase-index/" + state.index->id;
  Json blockers = Json::array();
  if (store.checkout_status()["pending"].get<bool>())
    blockers.push_back(Json{{"reason", "pending_checkout"}});
  if (changed)
    state.check_archive(archive, *state.index, blockers);
  const auto current = read_native_snapshot(store, state.head.id, options);
  auto incoming = read_native_snapshot(store, state.index->id, options);
  NativeFileMergeResult combined;
  if (changed) {
    const auto baseline =
        state.index_base ? read_native_snapshot(store, *state.index_base, options) : NativeSnapshot{};
    combined = merge_native_files(store, baseline, current, incoming, options);
    preserve_representations(baseline, current, incoming, combined.files, options);
  } else {
    combined.files = std::move(incoming);
  }
  if (!combined.conflicts.empty())
    blockers.push_back(Json{{"reason", "index_conflict"}});
  NativeStageRebasePlan plan;
  if (blockers.empty()) {
    if (changed) {
      plan.candidate = snapshot(state, combined.files, options);
      const auto encoded = plan.candidate.dump();
      validate_object(Kind::revision, bytes(encoded));
      plan.target = object_id(Kind::revision, bytes(encoded));
    } else {
      plan.target = state.index->id;
    }
  }
  auto difference = compare_native_file_maps(current, combined.files, options);
  if (verify_objects)
    (void)store.closure({state.head, *state.index}, true, options.stop, options.progress);
  const NativeResolutionSet context{"rebase-index",   state.head.id,    state.index->id,
                                    state.index_base, state.index_base, {}};
  plan.report = Json{{"version", 1},
                     {"operation", "rebase-index"},
                     {"branch", state.branch},
                     {"head", state.head.id},
                     {"index", state.index->id},
                     {"base", state.index_base ? Json(*state.index_base) : Json{}},
                     {"stale", changed},
                     {"changed", changed},
                     {"target", blockers.empty() ? Json(plan.target) : Json{}},
                     {"archive", changed ? Json(archive) : Json{}},
                     {"ready", blockers.empty()},
                     {"objects_verified", verify_objects},
                     {"result_files", blockers.empty() ? Json(combined.files.size()) : Json{}},
                     {"empty", difference["changes"].empty()},
                     {"counts", std::move(difference["counts"])},
                     {"changes", std::move(difference["changes"])},
                     {"conflicts", std::move(combined.conflicts)},
                     {"resolution_context", context.json()},
                     {"blockers", std::move(blockers)}};
  require(plan.report.dump().size() <= max_object_size, "native staging rebase report exceeds 16 MiB");
  report_progress(options, {ProgressPhase::planning, {}, 1, 1, true});
  return plan;
}
Json Store::plan_rebase_stage(std::string_view branch, bool verify_objects, OperationOptions options) const {
  const NativeRewriteState state(*this, branch, options);
  return prepare_native_stage_rebase(*this, state, verify_objects, options).report;
}
Json Store::rebase_stage(std::string_view branch, std::string_view expected, std::string_view expected_index,
                         OperationOptions options) const {
  throw_if_cancelled(options.stop);
  validate_id(expected);
  validate_id(expected_index);
  const NativeRewriteState state(*this, branch, options);
  require(state.head.id == expected, "native staging rebase tip changed; refresh the plan");
  require(state.index && state.index->id == expected_index,
          "native staging rebase index changed; refresh the plan");
  auto plan = prepare_native_stage_rebase(*this, state, true, options);
  require(plan.report["ready"].get<bool>(),
          "native staging rebase is blocked; inspect native rebase-index-plan");
  const bool changed = plan.report["changed"].get<bool>();
  if (changed) {
    throw_if_cancelled(options.stop);
    const auto next = put(Kind::revision, bytes(plan.candidate.dump())).first;
    require(next.id == plan.target, "native staging rebase candidate changed; refresh the plan");
    const auto archive = plan.report["archive"].get<std::string>();
    require(compare_update_refs({{state.head_name, state.head, state.head},
                                 {state.index_name, state.index, next},
                                 {archive, state.lookup(archive), state.index}},
                                options),
            "native staging references changed; retry operation");
  }
  plan.report["previous_index"] = state.index->id;
  plan.report["index"] = plan.target;
  plan.report["state"] = changed ? "applied" : "unchanged";
  return plan.report;
}
} // namespace syrnck
