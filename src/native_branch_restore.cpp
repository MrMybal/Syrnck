#include "native_branch_archive.hpp"
namespace syrnck {
namespace {
struct RestorePlan {
  NativeBranchArchive archive;
  std::string name;
  std::optional<ObjectKey> index;
  Json candidate, report;
};
RestorePlan prepare_restore(const Store &store, const Refs &refs, std::string_view id,
                            const std::optional<std::string> &name, bool verify, OperationOptions options) {
  RestorePlan plan;
  plan.archive = read_native_branch_archive(store, refs, id, options);
  auto source = plan.archive.state(store, options);
  plan.name = name.value_or(source.name);
  native_branch_name(plan.name);
  plan.index = source.index;
  const bool rewrite = source.index && plan.name != source.name;
  if (rewrite) {
    plan.candidate = std::move(source.prepared);
    plan.candidate["message"] = "Syrnck staging v1: " + plan.name;
    const auto encoded = plan.candidate.dump();
    validate_object(Kind::revision, bytes(encoded));
    plan.index = ObjectKey{Kind::revision, object_id(Kind::revision, bytes(encoded))};
  }
  const auto head = store.native_head();
  Json blockers = Json::array();
  if (store.checkout_status()["pending"].get<bool>())
    blockers.push_back(Json{{"reason", "pending_checkout"}});
  if (refs.contains("heads/" + plan.name))
    blockers.push_back(Json{{"reason", "destination_branch_exists"}});
  if (refs.contains("staging/" + plan.name))
    blockers.push_back(Json{{"reason", "destination_staging_exists"}});
  if (verify) {
    std::vector<ObjectKey> roots;
    for (const auto &[ref, key] : plan.archive.roots()) {
      (void)ref;
      roots.push_back(key);
    }
    (void)store.closure(roots, true, options.stop, options.progress);
  }
  plan.report = Json{{"version", 1},
                     {"operation", "branch-restore"},
                     {"archive", id},
                     {"original_name", source.name},
                     {"name", plan.name},
                     {"revision", source.target.id},
                     {"previous_index", source.index ? Json(source.index->id) : Json{}},
                     {"index", plan.index ? Json(plan.index->id) : Json{}},
                     {"index_rewritten", rewrite},
                     {"index_base", source.base ? Json(*source.base) : Json{}},
                     {"stale_index", source.index && source.base != source.target.id},
                     {"current", head["branch"].get<std::string>() == plan.name},
                     {"ready", blockers.empty()},
                     {"objects_verified", verify},
                     {"blockers", std::move(blockers)}};
  require(plan.report.dump().size() <= max_object_size, "native branch report exceeds 16 MiB");
  report_progress(options, {ProgressPhase::planning, plan.name, 1, 1, true});
  return plan;
}
} // namespace
Json Store::plan_restore_native_branch(std::string_view id, const std::optional<std::string> &name,
                                       bool verify_objects, OperationOptions options) const {
  throw_if_cancelled(options.stop);
  const auto all = refs();
  return prepare_restore(*this, all, id, name, verify_objects, options).report;
}
Json Store::restore_native_branch(std::string_view id, const std::optional<std::string> &name,
                                  OperationOptions options) const {
  throw_if_cancelled(options.stop);
  const auto all = refs();
  auto plan = prepare_restore(*this, all, id, name, true, options);
  require(plan.report["ready"].get<bool>(),
          "native branch restoration is blocked; inspect native branch-restore-plan");
  if (plan.report["index_rewritten"].get<bool>()) {
    throw_if_cancelled(options.stop);
    const auto candidate = put(Kind::revision, bytes(plan.candidate.dump())).first;
    require(plan.index == candidate, "native branch staging changed; refresh the plan");
  }
  const auto head = native_branch_lookup(all, "workspace/HEAD");
  auto updates = plan.archive.retention_updates(all);
  updates.push_back({"heads/" + plan.name, {}, plan.archive.target});
  updates.push_back({"staging/" + plan.name, {}, plan.index});
  updates.push_back({"workspace/HEAD", head, head});
  require(compare_update_refs(updates, options), "native branch references changed; retry operation");
  plan.report["state"] = "restored";
  return plan.report;
}
} // namespace syrnck
