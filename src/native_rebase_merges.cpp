#include "native_rebase.hpp"
#include "native_rewrite.hpp"
#include "native_authorship.hpp"
#include "native_parent_merge.hpp"
#include "native_virtual_snapshots.hpp"
#include <algorithm>
namespace syrnck {
namespace {
Json candidate(const Store &store, Json metadata, const std::vector<std::string> &parents,
               const NativeSnapshot &files, std::optional<std::uint64_t> clock,
               const std::function<void(std::size_t)> &work, OperationOptions options) {
  const auto author = native_original_author(metadata);
  Json tree = Json::object();
  auto &entries = tree.get_ref<Json::object_t &>();
  entries.reserve(files.size());
  for (const auto &[path, file] : files) {
    throw_if_cancelled(options.stop);
    work(1);
    entries.emplace_back(path, Json{{"manifest", file.manifest}, {"executable", file.executable}});
  }
  metadata["parents"] = parents;
  metadata["files"] = std::move(tree);
  if (clock)
    metadata["timestamp"] = *clock;
  return store.native_commit_metadata(std::move(metadata), author);
}
void fold_message(Json &metadata, const Json &source, bool squash) {
  if (!squash)
    return;
  const auto before = metadata["message"].get<std::string>(), next = source["message"].get<std::string>();
  require(before.size() <= 1024 * 1024 && next.size() <= 1024 * 1024 - before.size() &&
              before.size() + next.size() <= 1024 * 1024 - 2,
          "native rebase message exceeds 1 MiB");
  metadata["message"] = before + "\n\n" + next;
}
} // namespace
NativeRebasePlan prepare_native_rebase_merges(const Store &store, const fs::path &directory,
                                              std::string_view branch, std::string_view base,
                                              std::string_view onto, const NativeRebaseOptions &settings,
                                              bool verify_objects, OperationOptions options,
                                              const NativeRebaseSink &sink,
                                              std::optional<std::uint64_t> frozen_clock) {
  throw_if_cancelled(options.stop);
  require(settings.max_commits >= 1 && settings.max_commits <= 100000,
          "native rebase commit limit must be in 1..100000");
  require(settings.max_graph_work >= 1 && settings.max_graph_work <= 1024 * 1024 * 1024,
          "native rebase graph work limit must be in 1..1073741824");
  settings.base_limits.validate();
  validate_id(base);
  validate_id(onto);
  const bool identified = !store.native_identity().is_null();
  require(identified || !settings.committer_timestamp, "native committer timestamp requires an identity");
  const auto clock =
      identified ? std::optional{frozen_clock.value_or(settings.committer_timestamp.value_or(unix_seconds()))}
                 : std::optional<std::uint64_t>{};
  const NativeRewriteState state(store, branch, options);
  const auto selected = store.native_head();
  require(selected["directory"].is_null() ||
              utf8(fs::canonical(directory)) == selected["directory"].get<std::string>(),
          "native HEAD belongs to another workspace");
  std::size_t used_work = 0;
  const auto work = [&](std::size_t amount) {
    throw_if_cancelled(options.stop);
    require(amount <= settings.max_graph_work - used_work, "native rebase graph work limit exceeded");
    used_work += amount;
  };
  NativeAncestryGraph graph(
      store, {state.head.id, std::string(base), std::string(onto)}, settings.graph_limits, options,
      [&](const Json &value) { work(1 + value["parents"].size() + value["files"].size()); });
  const auto visited = graph.size(), links = graph.parent_links();
  const bool ancestor = graph.compare({std::string(base)}, {state.head.id}, options, work).source_is_ancestor;
  const auto range =
      ancestor ? graph.range(base, state.head.id, settings.max_commits, options) : std::vector<std::string>{};
  const std::set<std::string> in_range(range.begin(), range.end());
  NativeRebaseTodo sequence{state.head.id, std::string(base), std::string(onto), {}};
  for (const auto &id : range)
    sequence.steps.push_back({id, graph.parents(id).size() > 1 ? "merge" : "pick", {}});
  if (settings.todo) {
    settings.todo->validate();
    require(settings.todo->head == state.head.id && settings.todo->base == base &&
                settings.todo->onto == onto,
            "native rebase todo context changed; refresh the plan");
    require(ancestor && settings.todo->steps.size() == range.size(),
            "native merge-preserving todo must name every commit in the range");
    std::set<std::string> processed;
    for (const auto &step : settings.todo->steps) {
      require(in_range.contains(step.source), "native rebase todo source is outside the range");
      for (const auto &parent : graph.parents(step.source))
        require(!in_range.contains(parent) || processed.contains(parent),
                "native merge-preserving todo must keep parents before children");
      require(step.action != "merge" || graph.parents(step.source).size() > 1,
              "native merge todo action requires a merge revision");
      processed.insert(step.source);
    }
    sequence = *settings.todo;
  }
  if (settings.resolutions) {
    settings.resolutions->validate();
    require(settings.resolutions->head == state.head.id && settings.resolutions->base == base &&
                settings.resolutions->onto == onto,
            "native rebase resolution context changed; refresh the plan");
    for (const auto &[source, resolution] : settings.resolutions->steps) {
      (void)resolution;
      require(in_range.contains(source), "native rebase resolution source is outside the range");
    }
  }
  Json blockers = Json::array();
  if (!ancestor)
    blockers.push_back(Json{{"reason", "base_not_ancestor"}});
  if (state.index)
    blockers.push_back(Json{{"reason", "staging_exists"}});
  if (store.checkout_status()["pending"].get<bool>())
    blockers.push_back(Json{{"reason", "pending_checkout"}});
  const std::string_view mode = base == onto && !settings.todo && !settings.rebase_cousins ? "up_to_date"
                                : state.head.id == base                                    ? "move"
                                                                                           : "rebase";
  require(!settings.resolutions || mode != "up_to_date" || settings.resolutions->steps.empty(),
          "native rebase resolutions have no replay");
  for (const auto &todo : sequence.steps)
    require(todo.action != "drop" || !settings.resolutions ||
                !settings.resolutions->steps.contains(todo.source),
            "native rebase resolutions cannot target a dropped commit");
  std::optional<NativeTextMergeBatch> text;
  if (settings.text_merge)
    text.emplace(store, *settings.text_merge);
  std::optional<NativeRenameBatch> renames;
  if (settings.renames)
    renames.emplace(store, *settings.renames, text ? &text->generated : nullptr);
  NativeVirtualSnapshots snapshots(store, settings.max_delta_bytes, options, work);
  const NativeSnapshotReader read = [&](std::string_view id, OperationOptions operation) {
    throw_if_cancelled(operation.stop);
    return snapshots.read(id);
  };
  NativeRebasePlan plan{state.head.id, std::string(onto), {}, {}};
  const NativeRebaseResolutionSet context{state.head.id, std::string(base), std::string(onto), {}};
  Json rows = Json::array(), workspace;
  std::map<std::string, std::optional<std::string>> mapped;
  std::map<std::string, std::size_t> remaining, future_uses;
  for (const auto &id : range)
    for (const auto &parent : graph.parents(id))
      if (in_range.contains(parent))
        ++remaining[parent];
  auto target_for = [&](const std::string &id) -> std::optional<std::string> {
    if (const auto found = mapped.find(id); found != mapped.end())
      return found->second;
    require(!in_range.contains(id), "native merge-preserving todo must keep parents before children");
    return id == base || settings.rebase_cousins ? std::string(onto) : id;
  };
  auto remember = [&](const std::string &id, std::optional<std::string> target) {
    mapped[id] = target;
    if (target)
      future_uses[*target] += remaining[id];
  };
  std::optional<Json> pending;
  std::string pending_id;
  std::vector<std::string> pending_sources;
  std::set<std::string> generated_needed;
  const auto flush = [&] {
    if (!pending)
      return;
    if (text)
      for (const auto &[path, file] : (*pending)["files"].items()) {
        (void)path;
        work(1);
        const auto id = file["manifest"].get<std::string>();
        if (text->generated.contains(id))
          generated_needed.insert(id);
      }
    if (sink)
      sink(*pending, pending_id);
    pending.reset();
    pending_id.clear();
    pending_sources.clear();
  };
  std::set<ObjectKey> extra_roots;
  std::size_t completed = 0, created = 0, dropped = 0, merges = 0, budget = 4096;
  if (blockers.empty()) {
    if (mode == "up_to_date") {
      plan.target = plan.head;
      plan.files = read(plan.head, options);
    } else if (mode == "move")
      plan.files = read(onto, options);
    else {
      for (const auto &todo : sequence.steps) {
        work(1);
        const auto &source = todo.source;
        const auto parents = graph.parents(source);
        std::vector<std::optional<std::string>> parent_slots;
        std::vector<std::string> new_parents;
        for (const auto &parent : parents) {
          const auto target = target_for(parent);
          parent_slots.push_back(target);
          if (target)
            new_parents.push_back(*target);
          if (in_range.contains(parent)) {
            require(remaining[parent] > 0 && (!target || future_uses[*target] > 0),
                    "invalid native rebase parent accounting");
            --remaining[parent];
            if (target)
              --future_uses[*target];
          }
        }
        if (parents.empty() && settings.rebase_cousins)
          new_parents.push_back(std::string(onto));
        const auto first = parents.empty()
                               ? (settings.rebase_cousins ? std::optional{std::string(onto)} : std::nullopt)
                               : parent_slots.front();
        if (todo.action == "drop") {
          remember(source, first);
          if (first && pending && *first == pending_id)
            pending_sources.push_back(source);
          Json row{{"source", source},
                   {"action", "drop"},
                   {"parents", parents},
                   {"mapped_parents", parent_slots},
                   {"target", first ? Json(*first) : Json{}},
                   {"empty", true},
                   {"changes", Json::array()},
                   {"conflicts", Json::array()},
                   {"resolved", Json::array()}};
          const auto cost = row.dump().size() + 1;
          require(cost <= max_object_size - budget, "native rebase report exceeds 16 MiB");
          budget += cost;
          rows.push_back(std::move(row));
          ++dropped;
          continue;
        }
        const auto revision = store.metadata(Kind::revision, source);
        auto incoming = read(source, options);
        NativeSnapshot original, current;
        std::set<std::string> uncertain, forced, reused;
        std::optional<NativeParentMerge> old_merge, new_merge;
        std::optional<std::string> step_base, step_parent;
        std::string step_head;
        if (parents.size() > 1) {
          old_merge =
              merge_native_parents(store, graph, parents, settings.base_limits, read, text ? &*text : nullptr,
                                   renames ? &*renames : nullptr, work, options);
          new_merge =
              merge_native_parents(store, graph, new_parents, settings.base_limits, read,
                                   text ? &*text : nullptr, renames ? &*renames : nullptr, work, options);
          for (const auto &path : old_merge->reusable)
            if (new_merge->reusable.contains(path) &&
                old_merge->provenance.at(path) == new_merge->provenance.at(path))
              reused.insert(path);
          uncertain = old_merge->ambiguous;
          forced = new_merge->ambiguous;
          for (const auto &path : reused) {
            uncertain.erase(path);
            forced.erase(path);
          }
          step_base = old_merge->id;
          step_parent = parents.front();
          step_head = new_merge->id;
          original = std::move(old_merge->files);
          current = std::move(new_merge->files);
        } else {
          if (!parents.empty()) {
            step_base = parents.front();
            step_parent = parents.front();
            original = read(parents.front(), options);
          }
          if (!new_parents.empty()) {
            step_head = new_parents.front();
            current = read(step_head, options);
          } else
            step_head = merge_native_parents(store, graph, {}, settings.base_limits, read, nullptr, nullptr,
                                             work, options)
                            .id;
        }
        const NativeResolutionSet step_context{"rebase", step_head, source, step_base, step_parent, {}};
        const NativeResolutionSet *resolution = nullptr;
        if (settings.resolutions)
          if (const auto at = settings.resolutions->steps.find(source);
              at != settings.resolutions->steps.end()) {
            resolution = &at->second;
            require_native_resolution_context(*resolution, "rebase", step_head, source, step_base,
                                              step_parent);
          }
        work(original.size());
        work(current.size());
        work(incoming.size());
        auto combined = merge_native_files(store, original, current, incoming, options, resolution, nullptr,
                                           text ? &*text : nullptr, nullptr, renames ? &*renames : nullptr,
                                           false, &uncertain, &forced);
        extra_roots.insert(combined.extra_roots.begin(), combined.extra_roots.end());
        Json row{{"source", source},
                 {"action", todo.action},
                 {"kind", parents.size() > 1 ? "merge" : "commit"},
                 {"head", step_head},
                 {"parents", parents},
                 {"mapped_parents", parent_slots},
                 {"empty", combined.changes.empty()},
                 {"changes", std::move(combined.changes)},
                 {"conflicts", std::move(combined.conflicts)},
                 {"resolved", std::move(combined.resolved)},
                 {"resolution_context", step_context.json()}};
        if (old_merge) {
          row["original_parent_merge"] = std::move(old_merge->report);
          row["recreated_parent_merge"] = std::move(new_merge->report);
          row["recorded_resolutions_reused"] = reused;
        }
        if (text)
          row["text_merge"] = std::move(combined.text_merge);
        if (renames)
          row["renames"] = std::move(combined.renames);
        if (row["conflicts"].empty()) {
          const bool fold = todo.action == "squash" || todo.action == "fixup";
          auto metadata = revision;
          if (fold) {
            require(pending && !new_parents.empty() && new_parents.front() == pending_id &&
                        future_uses[pending_id] == 0,
                    "native rebase fold would change a branch point");
            metadata = *pending;
            auto attach = metadata["parents"].get<std::vector<std::string>>();
            attach.insert(attach.end(), new_parents.begin() + 1, new_parents.end());
            if (new_parents.size() > 1)
              work(new_parents.size());
            if (metadata["parents"].size() > 1)
              --merges;
            new_parents = std::move(attach);
            fold_message(metadata, revision, todo.action == "squash");
            row["replaces"] = pending_id;
          } else {
            flush();
            ++created;
            if (todo.message)
              metadata["message"] = *todo.message;
          }
          auto next =
              candidate(store, std::move(metadata), new_parents, combined.files, clock, work, options);
          const auto encoded = next.dump();
          validate_object(Kind::revision, bytes(encoded));
          const auto target = object_id(Kind::revision, bytes(encoded));
          if (!graph.contains(target)) {
            snapshots.add(target, new_parents.empty() ? std::nullopt : std::optional{new_parents.front()},
                          combined.files);
            graph.append(target, new_parents, options);
          } else
            graph.append(target, new_parents, options);
          if (fold)
            for (const auto &alias : pending_sources)
              mapped[alias] = target;
          remember(source, target);
          if (new_parents.size() > 1)
            ++merges;
          pending = std::move(next);
          pending_id = target;
          pending_sources.push_back(source);
          row["target"] = target;
          row["result_parents"] = new_parents;
          ++completed;
        } else
          row["target"] = Json{};
        const auto cost = row.dump().size() + 1;
        require(cost <= max_object_size - budget, "native rebase report exceeds 16 MiB");
        budget += cost;
        const bool conflict = !row["conflicts"].empty();
        rows.push_back(std::move(row));
        if (conflict) {
          blockers.push_back(Json{{"reason", "replay_conflict"}, {"source", source}});
          break;
        }
      }
      if (blockers.empty()) {
        flush();
        plan.target = mapped.at(plan.head).value_or(std::string(onto));
        plan.files = read(plan.target, options);
      }
    }
    if (blockers.empty())
      workspace = plan_native_checkout_files(store, directory, branch, plan.files, options);
  }
  if (verify_objects) {
    extra_roots.insert(state.head);
    extra_roots.insert({Kind::revision, std::string(base)});
    extra_roots.insert({Kind::revision, std::string(onto)});
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
                     {"preserve_merges", true},
                     {"rebase_cousins", settings.rebase_cousins},
                     {"max_commits", settings.max_commits},
                     {"commits", range},
                     {"steps", std::move(rows)},
                     {"completed_commits", completed},
                     {"result_commits", created},
                     {"result_merges", merges},
                     {"dropped_commits", dropped},
                     {"todo", ancestor ? sequence.json() : Json{}},
                     {"target", blockers.empty() ? Json(plan.target) : Json{}},
                     {"result_files", blockers.empty() ? Json(plan.files.size()) : Json{}},
                     {"ready", ready},
                     {"objects_verified", verify_objects},
                     {"resolution_context", context.json()},
                     {"blockers", std::move(blockers)},
                     {"workspace", std::move(workspace)},
                     {"visited_revisions", visited},
                     {"parent_links", links},
                     {"graph_work", used_work},
                     {"max_graph_work", settings.max_graph_work},
                     {"virtual_snapshots", snapshots.report()}};
  if (clock)
    plan.report["committer_timestamp"] = *clock;
  if (renames)
    plan.report["rename_detection"] = renames->report();
  if (text) {
    plan.report["text_merge"] = text->summary();
    for (const auto &id : generated_needed)
      plan.generated.emplace(id, std::move(text->generated.at(id)));
  }
  require(plan.report.dump().size() <= max_object_size, "native rebase report exceeds 16 MiB");
  report_progress(options, {ProgressPhase::planning, {}, completed, range.size(), true});
  return plan;
}
} // namespace syrnck
