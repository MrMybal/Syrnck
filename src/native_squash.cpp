#include "native_rewrite.hpp"
namespace syrnck {
namespace {
Json prepare_squash(const Store &store, const NativeRewriteState &state, std::size_t count,
                    bool verify_objects, OperationOptions options) {
  require(count >= 2 && count <= 100000, "native squash count must be in 2..100000");
  Json blockers = Json::array(), revisions = Json::array(), parents;
  if (store.checkout_status()["pending"].get<bool>())
    blockers.push_back(Json{{"reason", "pending_checkout"}});
  if (state.index)
    blockers.push_back(Json{{"reason", "staging_exists"}});
  state.check_archive(state.archive("squash"), state.head, blockers);
  auto id = state.head.id;
  auto value = state.revision;
  for (std::size_t n = 0; n < count; ++n) {
    throw_if_cancelled(options.stop);
    revisions.push_back(id);
    if (value["parents"].size() > 1) {
      blockers.push_back(Json{{"reason", "merge_in_range"}, {"revision", id}});
      break;
    }
    if (n + 1 == count) {
      parents = value["parents"];
      break;
    }
    if (value["parents"].empty()) {
      blockers.push_back(Json{{"reason", "count_exceeds_history"}});
      break;
    }
    id = value["parents"][0].get<std::string>();
    value = store.metadata(Kind::revision, id);
    if ((n + 1) % 256 == 0)
      report_progress(options, {ProgressPhase::planning, {}, n + 1, count});
  }
  if (verify_objects)
    (void)store.closure({state.head}, true, options.stop, options.progress);
  Json result{{"version", 1},
              {"operation", "squash"},
              {"branch", state.branch},
              {"head", state.head.id},
              {"count", count},
              {"revisions", std::move(revisions)},
              {"parents", std::move(parents)},
              {"result_files", state.revision["files"].size()},
              {"archive", state.archive("squash")},
              {"ready", blockers.empty()},
              {"objects_verified", verify_objects},
              {"blockers", std::move(blockers)}};
  require(result.dump().size() <= max_object_size, "native squash report exceeds 16 MiB");
  report_progress(options, {ProgressPhase::planning, {}, result["revisions"].size(), count, true});
  return result;
}
} // namespace
Json Store::plan_squash(std::string_view branch, std::size_t count, bool verify_objects,
                        OperationOptions options) const {
  throw_if_cancelled(options.stop);
  const NativeRewriteState state(*this, branch, options);
  return prepare_squash(*this, state, count, verify_objects, options);
}
Json Store::squash_commits(std::string_view branch, std::string_view expected, std::size_t count,
                           std::string_view message, OperationOptions options) const {
  throw_if_cancelled(options.stop);
  validate_id(expected);
  const NativeRewriteState state(*this, branch, options);
  require(state.head.id == expected, "native squash tip changed; inspect the branch and retry");
  auto report = prepare_squash(*this, state, count, true, options);
  require(report["ready"].get<bool>(), "native squash is blocked; inspect native squash-plan");
  auto revision = state.revision;
  revision["parents"] = report["parents"];
  revision["message"] = message;
  revision["timestamp"] = unix_seconds();
  throw_if_cancelled(options.stop);
  const auto next = put(Kind::revision, bytes(native_commit_metadata(std::move(revision)).dump())).first;
  state.publish(*this, "squash", next, {}, options);
  report["target"] = next.id;
  report["state"] = "applied";
  return report;
}
} // namespace syrnck
