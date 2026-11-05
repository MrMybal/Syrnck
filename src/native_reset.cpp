#include "native_rewrite.hpp"
namespace syrnck {
namespace {
std::string_view mode_name(NativeResetMode mode) {
  require(mode == NativeResetMode::soft || mode == NativeResetMode::mixed, "invalid native reset mode");
  return mode == NativeResetMode::soft ? "soft" : "mixed";
}
Json prepare_reset(const Store &store, const NativeRewriteState &state, std::string_view target,
                   NativeResetMode mode, bool verify_objects, OperationOptions options) {
  const auto name = mode_name(mode);
  validate_id(target);
  const auto destination = store.metadata(Kind::revision, target);
  const bool changed = state.head.id != target || (mode == NativeResetMode::mixed && state.index);
  Json blockers = Json::array();
  if (store.checkout_status()["pending"].get<bool>())
    blockers.push_back(Json{{"reason", "pending_checkout"}});
  if (mode == NativeResetMode::soft && state.index && state.index_base != state.head.id)
    blockers.push_back(Json{{"reason", "stale_index"}});
  if (changed) {
    state.check_archive(state.archive("reset"), state.head, blockers);
    if (state.index)
      state.check_archive(state.index_archive(), *state.index, blockers);
  }
  Json staged;
  if (mode == NativeResetMode::soft)
    staged = compare_native_snapshots(store, std::string(target),
                                      state.index ? state.index->id : state.head.id, options);
  if (verify_objects) {
    std::vector<ObjectKey> roots{state.head, {Kind::revision, std::string(target)}};
    if (state.index)
      roots.push_back(*state.index);
    (void)store.closure(roots, true, options.stop, options.progress);
  }
  Json result{{"version", 1},
              {"operation", "reset"},
              {"mode", name},
              {"branch", state.branch},
              {"head", state.head.id},
              {"target", target},
              {"index", state.index ? Json(state.index->id) : Json{}},
              {"index_action", !changed                         ? "unchanged"
                               : mode == NativeResetMode::mixed ? "remove"
                                                                : "rebase"},
              {"changed", changed},
              {"result_files", destination["files"].size()},
              {"staged", std::move(staged)},
              {"archive", changed ? Json(state.archive("reset")) : Json{}},
              {"index_archive", changed && state.index ? Json(state.index_archive()) : Json{}},
              {"ready", blockers.empty()},
              {"objects_verified", verify_objects},
              {"blockers", std::move(blockers)}};
  require(result.dump().size() <= max_object_size, "native reset report exceeds 16 MiB");
  report_progress(options, {ProgressPhase::planning, {}, 1, 1, true});
  return result;
}
} // namespace
Json Store::plan_reset_branch(std::string_view branch, std::string_view target, NativeResetMode mode,
                              bool verify_objects, OperationOptions options) const {
  throw_if_cancelled(options.stop);
  const NativeRewriteState state(*this, branch, options);
  return prepare_reset(*this, state, target, mode, verify_objects, options);
}
Json Store::reset_branch(std::string_view branch, std::string_view expected, std::string_view target,
                         NativeResetMode mode, OperationOptions options) const {
  throw_if_cancelled(options.stop);
  validate_id(expected);
  const NativeRewriteState state(*this, branch, options);
  require(state.head.id == expected, "native reset tip changed; inspect the branch and retry");
  auto report = prepare_reset(*this, state, target, mode, true, options);
  require(report["ready"].get<bool>(), "native reset is blocked; inspect native reset-plan");
  auto next_index = state.index;
  if (report["changed"].get<bool>()) {
    next_index.reset();
    if (mode == NativeResetMode::soft) {
      auto value = state.index ? state.prepared : state.revision;
      value["parents"] = Json::array({std::string(target)});
      value["message"] = "Syrnck staging v1: " + state.branch;
      value["timestamp"] = 0;
      throw_if_cancelled(options.stop);
      next_index = put(Kind::revision, bytes(value.dump())).first;
    }
    state.publish(*this, "reset", {Kind::revision, std::string(target)}, next_index, options);
  }
  report["previous_index"] = report["index"];
  report["index"] = next_index ? Json(next_index->id) : Json{};
  report["state"] = report["changed"].get<bool>() ? "applied" : "unchanged";
  return report;
}
} // namespace syrnck
