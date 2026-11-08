#include "native_replay.hpp"
#include "native_merge.hpp"
namespace syrnck {
NativeReplayPlan prepare_native_replay(const Store &store, const fs::path &directory, std::string_view branch,
                                       std::string_view source_id, NativeReplayKind kind,
                                       std::optional<std::uint32_t> mainline, bool verify_objects,
                                       OperationOptions options, const NativeResolutionSet *resolutions,
                                       const TextDiffOptions *text_limits) {
  throw_if_cancelled(options.stop);
  if (text_limits)
    text_limits->validate();
  require(kind == NativeReplayKind::cherry_pick || kind == NativeReplayKind::revert,
          "invalid native replay kind");
  validate_ref(native_revision_reference(std::string(branch)));
  validate_id(source_id);
  const auto selected_head = store.native_head();
  require(selected_head["directory"].is_null() ||
              utf8(fs::canonical(directory)) == selected_head["directory"].get<std::string>(),
          "native HEAD belongs to another workspace");
  const auto refs = store.refs();
  const auto head = refs.find(native_revision_reference(std::string(branch)));
  require(head != refs.end() && head->second.kind == Kind::revision, "unknown native branch");
  const auto revision = store.metadata(Kind::revision, source_id);
  const auto &parents = revision["parents"];
  require(parents.size() <= 1 || mainline.has_value(), "native replay of a merge requires --mainline");
  const auto selected = mainline.value_or(1);
  require((parents.empty() && !mainline) || (selected >= 1 && selected <= parents.size()),
          "native replay mainline is outside the parent list");
  const auto parent = parents.empty() ? std::optional<std::string>{}
                                      : std::optional{parents[selected - 1].get<std::string>()};
  const auto baseline = kind == NativeReplayKind::revert ? std::optional{std::string(source_id)} : parent;
  const NativeResolutionSet context{kind == NativeReplayKind::revert ? "revert" : "cherry-pick",
                                    head->second.id,
                                    std::string(source_id),
                                    baseline,
                                    parent,
                                    {}};
  if (resolutions)
    require_native_resolution_context(*resolutions, context.operation, context.head, source_id, baseline,
                                      parent);
  auto original = parent ? read_native_snapshot(store, *parent, options) : NativeSnapshot{};
  auto incoming = read_native_snapshot(store, source_id, options);
  if (kind == NativeReplayKind::revert)
    original.swap(incoming);
  const auto current = read_native_snapshot(store, head->second.id, options);
  NativeReplayPlan plan{head->second.id, {}, {}};
  auto combined = merge_native_files(store, original, current, incoming, options, resolutions, text_limits);
  plan.files = std::move(combined.files);
  plan.generated = std::move(combined.generated);
  auto &conflicts = combined.conflicts;
  auto &changes = combined.changes;
  Json workspace;
  if (conflicts.empty())
    workspace = plan_native_checkout_files(store, directory, branch, plan.files, options);
  if (verify_objects) {
    auto roots = combined.extra_roots;
    roots.push_back(head->second);
    roots.push_back({Kind::revision, std::string(source_id)});
    (void)store.closure(roots, true, options.stop, options.progress);
  }
  const bool ready = conflicts.empty() && !changes.empty() && workspace["ready"].get<bool>();
  plan.report = Json{{"version", 1},
                     {"operation", kind == NativeReplayKind::revert ? "revert" : "cherry-pick"},
                     {"branch", branch},
                     {"head", plan.head},
                     {"source", source_id},
                     {"parent", parent ? Json(*parent) : Json{}},
                     {"mainline", parents.empty() ? Json{} : Json(selected)},
                     {"ready", ready},
                     {"empty", changes.empty()},
                     {"objects_verified", verify_objects},
                     {"result_files", plan.files.size()},
                     {"changes", std::move(changes)},
                     {"conflicts", std::move(conflicts)},
                     {"resolved", std::move(combined.resolved)},
                     {"text_merge", std::move(combined.text_merge)},
                     {"resolution_context", context.json()},
                     {"workspace", std::move(workspace)}};
  require(plan.report.dump().size() <= max_object_size, "native replay report exceeds 16 MiB");
  report_progress(options, {ProgressPhase::planning, {}, combined.processed, combined.paths, true});
  return plan;
}
Json Store::plan_replay(const fs::path &directory, std::string_view branch, std::string_view source,
                        NativeReplayKind kind, std::optional<std::uint32_t> mainline, bool verify_objects,
                        OperationOptions options) const {
  return prepare_native_replay(*this, directory, branch, source, kind, mainline, verify_objects, options)
      .report;
}
Json Store::plan_replay_resolved(const fs::path &directory, std::string_view branch, std::string_view source,
                                 NativeReplayKind kind, const NativeResolutionSet &resolutions,
                                 std::optional<std::uint32_t> mainline, bool verify_objects,
                                 OperationOptions options) const {
  return prepare_native_replay(*this, directory, branch, source, kind, mainline, verify_objects, options,
                               &resolutions)
      .report;
}
Json Store::plan_replay_with_options(const fs::path &directory, std::string_view branch,
                                     std::string_view source, NativeReplayKind kind,
                                     const NativeReplayOptions &settings, bool verify_objects,
                                     OperationOptions options) const {
  return prepare_native_replay(*this, directory, branch, source, kind, settings.mainline, verify_objects,
                               options, settings.resolutions ? &*settings.resolutions : nullptr,
                               settings.text_merge ? &*settings.text_merge : nullptr)
      .report;
}
} // namespace syrnck
