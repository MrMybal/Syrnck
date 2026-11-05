#include "native_rebase.hpp"
#include "native_rewrite.hpp"
#include <algorithm>
namespace syrnck {
namespace {
Json candidate(Json revision, std::string_view parent, const NativeSnapshot &files,
               OperationOptions options) {
  Json entries = Json::object();
  auto &object = entries.get_ref<Json::object_t &>();
  object.reserve(files.size());
  for (const auto &[path, file] : files) {
    throw_if_cancelled(options.stop);
    object.emplace_back(path, Json{{"manifest", file.manifest}, {"executable", file.executable}});
  }
  revision["parents"] = Json::array({std::string(parent)});
  revision["files"] = std::move(entries);
  return revision;
}
} // namespace
NativeRebasePlan prepare_native_rebase(const Store &store, const fs::path &directory, std::string_view branch,
                                       std::string_view base, std::string_view onto,
                                       const NativeRebaseOptions &settings, bool verify_objects,
                                       OperationOptions options, const NativeRebaseSink &sink) {
  throw_if_cancelled(options.stop);
  require(settings.max_commits >= 1 && settings.max_commits <= 100000,
          "native rebase commit limit must be in 1..100000");
  validate_id(base);
  validate_id(onto);
  const NativeRewriteState state(store, branch, options);
  if (settings.resolutions) {
    settings.resolutions->validate();
    require(settings.resolutions->head == state.head.id && settings.resolutions->base == base &&
                settings.resolutions->onto == onto,
            "native rebase resolution context changed; refresh the plan");
  }
  const auto selected = store.native_head();
  require(selected["directory"].is_null() ||
              utf8(fs::canonical(directory)) == selected["directory"].get<std::string>(),
          "native HEAD belongs to another workspace");
  (void)store.metadata(Kind::revision, base);
  (void)store.metadata(Kind::revision, onto);
  Json blockers = Json::array();
  if (state.index)
    blockers.push_back(Json{{"reason", "staging_exists"}});
  if (store.checkout_status()["pending"].get<bool>())
    blockers.push_back(Json{{"reason", "pending_checkout"}});
  std::vector<std::string> range;
  std::set<std::string> seen;
  auto id = state.head.id;
  while (id != base) {
    throw_if_cancelled(options.stop);
    require(range.size() < settings.max_commits, "native rebase commit limit exceeded");
    require(seen.insert(id).second, "native rebase history contains a cycle");
    const auto revision = store.metadata(Kind::revision, id);
    const auto &parents = revision["parents"];
    if (parents.size() != 1) {
      seen.erase(id);
      blockers.push_back(Json{{"reason", parents.empty() ? "base_not_on_linear_history" : "merge_in_range"},
                              {"revision", id}});
      break;
    }
    range.push_back(id);
    id = parents[0].get<std::string>();
    if (range.size() % 256 == 0)
      report_progress(options, {ProgressPhase::planning, {}, range.size()});
  }
  std::reverse(range.begin(), range.end());
  const std::string_view mode = base == onto ? "up_to_date" : state.head.id == base ? "move" : "rebase";
  const NativeRebaseResolutionSet context{state.head.id, std::string(base), std::string(onto), {}};
  if (settings.resolutions) {
    require(mode != "up_to_date" || settings.resolutions->steps.empty(),
            "native rebase resolutions have no replay");
    for (const auto &[source, step] : settings.resolutions->steps) {
      (void)step;
      require(seen.contains(source), "native rebase resolution source is outside the range");
    }
  }
  NativeRebasePlan plan{state.head.id, std::string(onto), {}, {}};
  Json steps = Json::array(), workspace;
  std::size_t budget = 4096, completed = 0;
  std::set<ObjectKey> extra_roots;
  for (const auto &source : range)
    budget += source.size() + 3;
  require(budget <= max_object_size, "native rebase report exceeds 16 MiB");
  if (blockers.empty()) {
    if (mode == "up_to_date") {
      plan.target = state.head.id;
      plan.files = read_native_snapshot(store, state.head.id, options);
    } else {
      auto original = read_native_snapshot(store, base, options);
      auto current = read_native_snapshot(store, onto, options);
      for (const auto &source : range) {
        throw_if_cancelled(options.stop);
        auto incoming = read_native_snapshot(store, source, options);
        const auto revision = store.metadata(Kind::revision, source);
        const auto parent = revision["parents"][0].get<std::string>();
        const NativeResolutionSet step_context{"rebase", plan.target, source, parent, parent, {}};
        const NativeResolutionSet *resolution = nullptr;
        if (settings.resolutions) {
          const auto found = settings.resolutions->steps.find(source);
          if (found != settings.resolutions->steps.end()) {
            resolution = &found->second;
            require_native_resolution_context(*resolution, "rebase", plan.target, source, parent, parent);
          }
        }
        auto combined = merge_native_files(store, original, current, incoming, options, resolution);
        extra_roots.insert(combined.extra_roots.begin(), combined.extra_roots.end());
        Json row{{"source", source},
                 {"head", plan.target},
                 {"parent", revision["parents"][0]},
                 {"empty", combined.changes.empty()},
                 {"changes", std::move(combined.changes)},
                 {"conflicts", std::move(combined.conflicts)},
                 {"resolved", std::move(combined.resolved)},
                 {"resolution_context", step_context.json()}};
        if (row["conflicts"].empty()) {
          const auto next = candidate(revision, plan.target, combined.files, options);
          const auto encoded = next.dump();
          const auto data = bytes(encoded);
          validate_object(Kind::revision, data);
          plan.target = object_id(Kind::revision, data);
          row["target"] = plan.target;
          if (sink)
            sink(next, plan.target);
          ++completed;
        } else {
          row["target"] = Json{};
        }
        const auto size = row.dump().size() + 1;
        require(size <= max_object_size - budget, "native rebase report exceeds 16 MiB");
        budget += size;
        const bool conflicted = !row["conflicts"].empty();
        steps.push_back(std::move(row));
        if (conflicted) {
          blockers.push_back(Json{{"reason", "replay_conflict"}, {"source", source}});
          break;
        }
        current = std::move(combined.files);
        original = std::move(incoming);
      }
      plan.files = std::move(current);
    }
    if (blockers.empty())
      workspace = plan_native_checkout_files(store, directory, branch, plan.files, options);
  }
  if (verify_objects) {
    extra_roots.insert(state.head);
    extra_roots.insert({Kind::revision, std::string(onto)});
    extra_roots.insert({Kind::revision, std::string(base)});
    (void)store.closure(std::vector<ObjectKey>(extra_roots.begin(), extra_roots.end()), true, options.stop,
                        options.progress);
  }
  const bool ready = blockers.empty() && workspace["ready"].get<bool>();
  plan.report = Json{{"version", 1},
                     {"operation", "rebase"},
                     {"branch", branch},
                     {"head", plan.head},
                     {"base", base},
                     {"onto", onto},
                     {"mode", mode},
                     {"max_commits", settings.max_commits},
                     {"commits", range},
                     {"steps", std::move(steps)},
                     {"completed_commits", completed},
                     {"target", blockers.empty() ? Json(plan.target) : Json{}},
                     {"result_files", blockers.empty() ? Json(plan.files.size()) : Json{}},
                     {"ready", ready},
                     {"objects_verified", verify_objects},
                     {"resolution_context", context.json()},
                     {"blockers", std::move(blockers)},
                     {"workspace", std::move(workspace)}};
  require(plan.report.dump().size() <= max_object_size, "native rebase report exceeds 16 MiB");
  report_progress(options, {ProgressPhase::planning, {}, completed, range.size(), true});
  return plan;
}
Json Store::plan_rebase(const fs::path &directory, std::string_view branch, std::string_view base,
                        std::string_view onto, const NativeRebaseOptions &settings, bool verify_objects,
                        OperationOptions options) const {
  return prepare_native_rebase(*this, directory, branch, base, onto, settings, verify_objects, options)
      .report;
}
} // namespace syrnck
