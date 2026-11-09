#include "native_stage_rebase.hpp"
namespace syrnck {
namespace {
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
                                                  bool verify_objects, OperationOptions options,
                                                  const NativeResolutionSet *resolutions,
                                                  const TextDiffOptions *text_limits, const NativeRenameOptions *rename_limits) {
  throw_if_cancelled(options.stop);
  if (text_limits)
    text_limits->validate();
  require(state.index.has_value(), "no native staging snapshot");
  const NativeResolutionSet context{"rebase-index",   state.head.id,    state.index->id,
                                    state.index_base, state.index_base, {}};
  if (resolutions)
    require_native_resolution_context(*resolutions, context.operation, state.head.id, state.index->id,
                                      state.index_base, state.index_base);
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
  std::optional<NativeRenameBatch> renames;
  if (rename_limits) renames.emplace(store, *rename_limits);
  if (changed) {
    const auto baseline =
        state.index_base ? read_native_snapshot(store, *state.index_base, options) : NativeSnapshot{};
    combined = merge_native_files(store, baseline, current, incoming, options, resolutions, text_limits,
                                   nullptr, nullptr, renames ? &*renames : nullptr, true);
  } else {
    require(!resolutions || resolutions->files.empty(), "native resolution path has no conflict");
    combined.files = std::move(incoming);
    if (text_limits)
      combined.text_merge = NativeTextMergeBatch(store, *text_limits).report();
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
  if (verify_objects) {
    auto roots = combined.extra_roots;
    roots.push_back(state.head);
    roots.push_back(*state.index);
    (void)store.closure(roots, true, options.stop, options.progress);
  }
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
                     {"resolved", std::move(combined.resolved)},
                     {"resolution_context", context.json()},
                     {"blockers", std::move(blockers)}};
  if (text_limits)
    plan.report["text_merge"] = std::move(combined.text_merge);
  if (rename_limits) plan.report["renames"] = std::move(combined.renames);
  plan.generated = std::move(combined.generated);
  require(plan.report.dump().size() <= max_object_size, "native staging rebase report exceeds 16 MiB");
  report_progress(options, {ProgressPhase::planning, {}, 1, 1, true});
  return plan;
}
Json Store::plan_rebase_stage(std::string_view branch, bool verify_objects, OperationOptions options) const {
  const NativeRewriteState state(*this, branch, options);
  return prepare_native_stage_rebase(*this, state, verify_objects, options).report;
}
Json Store::plan_rebase_stage_resolved(std::string_view branch, const NativeResolutionSet &resolutions,
                                       bool verify_objects, OperationOptions options) const {
  const NativeRewriteState state(*this, branch, options);
  return prepare_native_stage_rebase(*this, state, verify_objects, options, &resolutions).report;
}
Json Store::plan_rebase_stage_with_options(std::string_view branch, const NativeStageRebaseOptions &settings,
                                           bool verify_objects, OperationOptions options) const {
  const NativeRewriteState state(*this, branch, options);
  return prepare_native_stage_rebase(*this, state, verify_objects, options,
                                     settings.resolutions ? &*settings.resolutions : nullptr,
                                     settings.text_merge ? &*settings.text_merge : nullptr,
                                     settings.renames ? &*settings.renames : nullptr)
      .report;
}
namespace {
Json apply(const Store &store, std::string_view branch, std::string_view expected,
           std::string_view expected_index, OperationOptions options, const NativeResolutionSet *resolutions,
           const TextDiffOptions *text_limits = nullptr, const NativeRenameOptions *rename_limits = nullptr) {
  throw_if_cancelled(options.stop);
  validate_id(expected);
  validate_id(expected_index);
  const NativeRewriteState state(store, branch, options);
  require(state.head.id == expected, "native staging rebase tip changed; refresh the plan");
  require(state.index && state.index->id == expected_index,
          "native staging rebase index changed; refresh the plan");
  auto plan = prepare_native_stage_rebase(store, state, true, options, resolutions, text_limits, rename_limits);
  require(plan.report["ready"].get<bool>(),
          "native staging rebase is blocked; inspect native rebase-index-plan");
  const bool changed = plan.report["changed"].get<bool>();
  plan.report["previous_index"] = state.index->id;
  plan.report["index"] = plan.target;
  plan.report["state"] = changed ? "applied" : "unchanged";
  if (changed) {
    throw_if_cancelled(options.stop);
    materialize_native_text(store, plan.generated, options);
    const auto next = store.put(Kind::revision, bytes(plan.candidate.dump())).first;
    require(next.id == plan.target, "native staging rebase candidate changed; refresh the plan");
    const auto archive = plan.report["archive"].get<std::string>();
    require(store.compare_update_refs({{state.head_name, state.head, state.head},
                                       {state.index_name, state.index, next},
                                       {archive, state.lookup(archive), state.index}},
                                      options),
            "native staging references changed; retry operation");
  }
  return plan.report;
}
} // namespace
Json Store::rebase_stage(std::string_view branch, std::string_view expected, std::string_view expected_index,
                         OperationOptions options) const {
  return apply(*this, branch, expected, expected_index, options, nullptr);
}
Json Store::rebase_stage_resolved(std::string_view branch, std::string_view expected,
                                  std::string_view expected_index, const NativeResolutionSet &resolutions,
                                  OperationOptions options) const {
  return apply(*this, branch, expected, expected_index, options, &resolutions);
}
Json Store::rebase_stage_with_options(std::string_view branch, std::string_view expected,
                                      std::string_view expected_index,
                                      const NativeStageRebaseOptions &settings,
                                      OperationOptions options) const {
  return apply(*this, branch, expected, expected_index, options,
               settings.resolutions ? &*settings.resolutions : nullptr,
               settings.text_merge ? &*settings.text_merge : nullptr,
               settings.renames ? &*settings.renames : nullptr);
}
} // namespace syrnck
