#include "native_stash.hpp"
#include "native_rewrite.hpp"
namespace syrnck {
namespace {
Json snapshot(const Json &parents, const std::string &message, const NativeSnapshot &files,
              OperationOptions options) {
  Json entries = Json::object();
  auto &object = entries.get_ref<Json::object_t &>();
  object.reserve(files.size());
  for (const auto &[path, file] : files) {
    throw_if_cancelled(options.stop);
    object.emplace_back(path, Json{{"manifest", file.manifest}, {"executable", file.executable}});
  }
  Json value{{"version", 1},
             {"parents", parents},
             {"message", message},
             {"timestamp", 0},
             {"files", std::move(entries)}};
  validate_object(Kind::revision, bytes(value.dump()));
  return value;
}
Json replay_report(const NativeFileMergeResult &result, const NativeResolutionSet &context) {
  Json report{{"changes", result.changes},
              {"conflicts", result.conflicts},
              {"resolved", result.resolved},
              {"resolution_context", context.json()}};
  if (!result.text_merge.is_null())
    report["text_merge"] = result.text_merge;
  return report;
}
} // namespace
Json native_stash_working_parents(std::string_view head, std::string_view work,
                                  const std::optional<std::string> &index) {
  Json parents = Json::array({std::string(head)});
  if (work != head)
    parents.push_back(std::string(work));
  if (index && *index != head && *index != work)
    parents.push_back(*index);
  return parents;
}
NativeStashApplyPlan prepare_native_stash_apply(const Store &store, const fs::path &directory,
                                                std::string_view branch, std::string_view id,
                                                const NativeStashApplyOptions &settings, bool verify_objects,
                                                OperationOptions options) {
  throw_if_cancelled(options.stop);
  const auto materialization = read_native_materialization(store);
  const NativeRewriteState state(store, branch, options);
  NativeStashApplyPlan plan;
  plan.saved = read_native_stash(store, id, false, options);
  plan.materialization = materialization;
  require(plan.saved.materialization.definition() == materialization.policy.definition(),
          "native stash materialization differs from workspace");
  plan.head = state.head.id;
  plan.selection = state.lookup(store.native_workspace_reference("workspace/HEAD"));
  const auto selection = store.native_head();
  require(store.native_target() == branch,
          "native stash apply requires the selected branch");
  require(selection["directory"].is_null() ||
              utf8(fs::canonical(directory)) == selection["directory"].get<std::string>(),
          "native HEAD belongs to another workspace");
  require(!settings.index_resolutions || (settings.restore_index && plan.saved.index),
          "native stash index resolutions require a saved index and --index");
  std::optional<NativeTextMergeBatch> text;
  if (settings.text_merge)
    text.emplace(store, *settings.text_merge);
  Json blockers = Json::array();
  if (state.index)
    blockers.push_back(Json{{"reason", "staging_exists"}});
  if (store.checkout_status()["pending"].get<bool>())
    blockers.push_back(Json{{"reason", "pending_checkout"}});
  const auto base = read_native_materialized_snapshot(store, plan.saved.base, materialization.policy, options);
  const auto current = read_native_materialized_snapshot(store, plan.head, materialization.policy, options);
  const auto incoming = read_native_snapshot(store, plan.saved.work, options);
  const NativeResolutionSet work_context{"stash",         plan.head,       plan.saved.work,
                                         plan.saved.base, plan.saved.base, {}};
  if (settings.resolutions)
    require_native_resolution_context(*settings.resolutions, "stash", plan.head, plan.saved.work,
                                      plan.saved.base, plan.saved.base);
  auto work = merge_native_files(store, base, current, incoming, options,
                                 settings.resolutions ? &*settings.resolutions : nullptr, nullptr,
                                 text ? &*text : nullptr);
  if (!work.conflicts.empty())
    blockers.push_back(Json{{"reason", "working_conflict"}});
  std::set<ObjectKey> extra(work.extra_roots.begin(), work.extra_roots.end());
  Json index_report;
  if (settings.restore_index && plan.saved.index) {
    const auto metadata = store.metadata(Kind::revision, *plan.saved.index);
    const auto original = metadata["parents"].empty()
                              ? std::optional<std::string>{}
                              : std::optional{metadata["parents"][0].get<std::string>()};
    const auto baseline = original ? read_native_snapshot(store, *original, options) : NativeSnapshot{};
    const auto staged = read_native_snapshot(store, *plan.saved.index, options);
    const NativeResolutionSet context{"stash-index", plan.head, *plan.saved.index, original, original, {}};
    if (settings.index_resolutions)
      require_native_resolution_context(*settings.index_resolutions, "stash-index", plan.head,
                                        *plan.saved.index, original, original);
    const auto logical_current = read_native_snapshot(store, plan.head, options);
    auto index = merge_native_files(store, baseline, logical_current, staged, options,
                                    settings.index_resolutions ? &*settings.index_resolutions : nullptr,
                                    nullptr, text ? &*text : nullptr);
    extra.insert(index.extra_roots.begin(), index.extra_roots.end());
    index_report = replay_report(index, context);
    if (!index.conflicts.empty())
      blockers.push_back(Json{{"reason", "index_conflict"}});
    else {
      plan.index_revision = snapshot(Json::array({plan.head}), "Syrnck staging v1: " + std::string(branch),
                                     index.files, options);
      plan.index = object_id(Kind::revision, bytes(plan.index_revision.dump()));
    }
  }
  plan.files = std::move(work.files);
  Json workspace;
  if (blockers.empty()) {
    plan.working_revision = snapshot(native_stash_working_parents(plan.head, plan.saved.work, plan.index),
                                     "Syrnck stash working v1: " + std::string(branch), plan.files, options);
    plan.target = object_id(Kind::revision, bytes(plan.working_revision.dump()));
    workspace = plan_native_checkout_files(store, directory, branch, plan.files, options, true);
  }
  if (verify_objects) {
    extra.insert({Kind::revision, plan.saved.work});
    extra.insert(state.head);
    (void)store.closure(std::vector<ObjectKey>(extra.begin(), extra.end()), true, options.stop,
                        options.progress);
  }
  const auto work_report = replay_report(work, work_context);
  const auto ready = blockers.empty() && workspace["ready"].get<bool>();
  const bool partial = !materialization.policy.complete();
  const auto version = partial ? 2 : 1;
  Json context{{"version", version},
                     {"type", "native-stash-apply-plan"},
                     {"directory", utf8(fs::canonical(directory))},
                     {"store", utf8(store.root())},
                     {"branch", branch},
                     {"head", plan.head},
                     {"stash", id},
                     {"record", plan.saved.record.id},
                     {"selection", plan.selection ? key_json(*plan.selection) : Json{}},
                     {"target", plan.target.empty() ? Json{} : Json(plan.target)},
                     {"target_index", plan.index ? Json(*plan.index) : Json{}},
                     {"restore_index", settings.restore_index}};
  if (partial) {
    context["materialization"] = materialization.policy.definition();
    context["materialization_object"] = key_json(*materialization.root);
  }
  plan.report = Json{{"version", version},
                     {"operation", "stash-apply"},
                     {"stash", id},
                     {"record", plan.saved.record.id},
                     {"head", plan.head},
                     {"base", plan.saved.base},
                     {"restore_index", settings.restore_index},
                     {"target", context["target"]},
                     {"target_index", context["target_index"]},
                     {"plan", hash(bytes(std::string(partial ? "Syrnck stash apply plan v2:" : "Syrnck stash apply plan v1:") + context.dump()))},
                     {"working", work_report},
                     {"index", std::move(index_report)},
                     {"workspace", std::move(workspace)},
                     {"ready", ready},
                     {"blockers", std::move(blockers)},
                     {"objects_verified", verify_objects}};
  if (partial) {
    plan.report["materialization"] = context["materialization"];
    plan.report["materialization_object"] = context["materialization_object"];
  }
  if (text) {
    plan.report["text_merge"] = text->summary();
    plan.generated = std::move(text->generated);
  }
  require(plan.report.dump().size() <= max_object_size, "native stash apply report exceeds 16 MiB");
  return plan;
}
Json NativeStash::plan_apply(const fs::path &directory, std::string_view branch, std::string_view id,
                             const NativeStashApplyOptions &settings, bool verify_objects,
                             OperationOptions options) const {
  return prepare_native_stash_apply(store_, directory, branch, id, settings, verify_objects, options).report;
}
Json NativeStash::apply(const fs::path &directory, std::string_view branch, std::string_view id,
                        std::string_view expected_plan, bool pop, const NativeStashApplyOptions &settings,
                        OperationOptions options) const {
  throw_if_cancelled(options.stop);
  validate_id(expected_plan);
  require_no_native_checkout(store_);
  auto plan = prepare_native_stash_apply(store_, directory, branch, id, settings, true, options);
  require(plan.report["plan"].get<std::string>() == expected_plan,
          "native stash apply plan changed; refresh the plan");
  require(plan.report["ready"].get<bool>(), "native stash apply is blocked; inspect the plan");
  materialize_native_text(store_, plan.generated, options);
  if (plan.index)
    require(store_.put(Kind::revision, bytes(plan.index_revision.dump())).first.id == *plan.index,
            "native stash candidate changed; refresh the plan");
  require(store_.put(Kind::revision, bytes(plan.working_revision.dump())).first.id == plan.target,
          "native stash candidate changed; refresh the plan");
  auto result = checkout_native_stash(store_, directory, branch, plan, pop, options);
  result["operation"] = pop ? "stash-pop" : "stash-apply";
  result["plan"] = expected_plan;
  result["stash"] = id;
  result["restored_index"] = plan.index ? Json(*plan.index) : Json{};
  return result;
}
} // namespace syrnck
