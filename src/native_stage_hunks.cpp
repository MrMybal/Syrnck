#include "native_branch.hpp"
#include "native_context.hpp"
#include "native_text_diff.hpp"
#include <algorithm>
#include <sstream>
namespace syrnck {
namespace {
Json nullable(const std::optional<ObjectKey> &key) {
  return key ? Json(key->id) : Json{};
}
struct HunkPlan {
  Refs refs;
  std::string branch, path, archive, source_text;
  std::optional<ObjectKey> head, index, base, source;
  Json snapshot, report;
  NativeWorkingDiff working;
};
HunkPlan state(const Store &store, std::string_view branch, std::string_view path, OperationOptions options) {
  throw_if_cancelled(options.stop);
  validate_native_target(branch);
  validate_path(path);
  HunkPlan plan;
  plan.refs = store.refs();
  plan.branch = branch;
  plan.path = path;
  plan.head = native_branch_lookup(plan.refs, native_revision_reference(store, plan.branch));
  plan.index = native_branch_lookup(plan.refs, native_staging_reference(store, plan.branch));
  require(!plan.head || plan.head->kind == Kind::revision, "branch must point to a revision");
  require(!plan.index || plan.index->kind == Kind::revision, "invalid native staging snapshot");
  plan.base = plan.head;
  plan.source = plan.index ? plan.index : plan.head;
  if (plan.source)
    plan.snapshot = store.metadata(Kind::revision, plan.source->id);
  if (plan.index) {
    require(plan.index->kind == Kind::revision &&
                plan.snapshot["message"] == "Syrnck staging v1: " + plan.branch &&
                plan.snapshot["timestamp"] == 0 && plan.snapshot["parents"].size() <= 1,
            "invalid native staging snapshot");
    plan.base =
        plan.snapshot["parents"].empty()
            ? std::optional<ObjectKey>{}
            : std::optional{ObjectKey{Kind::revision, plan.snapshot["parents"][0].get<std::string>()}};
    plan.archive = "archive/stage-index/" + plan.index->id;
  }
  return plan;
}
HunkPlan prepare(const Store &store, const fs::path *directory, std::string_view branch,
                 std::string_view path, TextDiffOptions limits, OperationOptions options,
                 bool by_line = false) {
  limits.validate();
  auto plan = state(store, branch, path, options);
  Json report;
  if (directory) {
    plan.working = read_native_working_diff(store, *directory,
                                            plan.source ? std::optional{plan.source->id} : std::nullopt, path,
                                            limits, options);
    report = std::move(plan.working.report);
    plan.source_text = std::move(plan.working.source);
  } else {
    auto frozen = read_native_revision_diff(store, plan.base ? std::optional{plan.base->id} : std::nullopt,
                                            plan.source ? std::optional{plan.source->id} : std::nullopt, path,
                                            limits, options);
    report = std::move(frozen.report);
    plan.source_text = std::move(frozen.source);
  }
  Json blockers = Json::array();
  if (store.checkout_status()["pending"].get<bool>())
    blockers.push_back(Json{{"reason", "pending_checkout"}});
  if (plan.head != plan.base)
    blockers.push_back(Json{{"reason", "stale_staging"}});
  if (!directory && !plan.index)
    blockers.push_back(Json{{"reason", "staging_required"}});
  if (report["source"].is_null() || report["target"].is_null())
    blockers.push_back(Json{{"reason", "existing_file_required"}});
  if (report["diff"]["status"] != "text")
    blockers.push_back(Json{{"reason", "text_diff_required"}});
  else if (report["diff"]["hunks"].empty())
    blockers.push_back(Json{{"reason", "no_text_changes"}});
  if (plan.index) {
    const auto existing = native_branch_lookup(plan.refs, plan.archive);
    if (existing && existing != plan.index)
      blockers.push_back(Json{{"reason", "archive_conflict"}, {"reference", plan.archive}});
  }
  std::size_t number = 0, changes = 0;
  for (auto &hunk : report["diff"]["hunks"]) {
    hunk["number"] = ++number;
    if (by_line)
      for (auto &line : hunk["lines"])
        if (line["kind"] != "context")
          line["change_number"] = ++changes;
  }
  if (by_line) {
    report["change_count"] = changes;
    report["selection_unit"] = "line";
    if (changes > 100000)
      blockers.push_back(Json{{"reason", "selection_limit"}});
    report["operation"] = directory ? "stage-lines" : "reset-lines";
  } else {
    report["operation"] = directory ? "stage-hunks" : "reset-hunks";
  }
  report["branch"] = branch;
  report["head"] = nullable(plan.head);
  report["index"] = nullable(plan.index);
  report["base"] = nullable(plan.base);
  report["stale"] = plan.head != plan.base;
  report["archive"] = plan.index ? Json(plan.archive) : Json{};
  report["executable_preserved"] = true;
  report["text_options"] = Json{{"context", limits.context},
                                {"max_file_bytes", limits.max_file_bytes},
                                {"max_lines", limits.max_lines},
                                {"max_work", limits.max_work},
                                {"max_output_bytes", limits.max_output_bytes}};
  report["ready"] = blockers.empty();
  report["blockers"] = std::move(blockers);
  const auto encoded = report.dump();
  require(encoded.size() + 100 <= max_object_size, "native text selection plan exceeds 16 MiB");
  report["plan_id"] = object_id(Kind::chunk, bytes(encoded));
  plan.report = std::move(report);
  return plan;
}
std::string select(const HunkPlan &plan, const std::set<std::size_t> &selected, TextDiffOptions limits,
                   OperationOptions options, bool by_line) {
  std::vector<std::string_view> lines;
  const std::string_view source(plan.source_text);
  for (std::size_t pos = 0; pos < source.size();) {
    const auto newline = source.find('\n', pos);
    const auto end = newline == source.npos ? source.size() : newline + 1;
    lines.push_back(source.substr(pos, end - pos));
    pos = end;
  }
  std::string result;
  std::size_t cursor = 0;
  auto append = [&](std::string_view text) {
    throw_if_cancelled(options.stop);
    require(!by_line || text.empty() || result.empty() || result.back() == '\n',
            "native line selection joins unterminated lines; select related changes together");
    require(text.size() <= limits.max_file_bytes - result.size(),
            "native text selection result exceeds file budget");
    result += text;
  };
  for (const auto &hunk : plan.report["diff"]["hunks"]) {
    if (!by_line && !selected.contains(hunk["number"].get<std::size_t>()))
      continue;
    const auto count = hunk["source_count"].get<std::size_t>();
    const auto start = hunk["source_start"].get<std::size_t>() - (count != 0);
    require(start >= cursor && start <= lines.size(), "invalid native hunk range");
    while (cursor < start)
      append(lines[cursor++]);
    for (const auto &line : hunk["lines"]) {
      auto text = line["text"].get<std::string>();
      if (line["newline"].get<bool>())
        text += '\n';
      if (line["kind"] != "add") {
        require(cursor < lines.size() && lines[cursor] == text,
                "native text selection source changed; refresh the plan");
        ++cursor;
      }
      const bool chosen = !by_line || (line.contains("change_number") &&
                                       selected.contains(line["change_number"].get<std::size_t>()));
      if (line["kind"] == "context" || (line["kind"] == "add" && chosen) ||
          (line["kind"] == "remove" && !chosen))
        append(text);
    }
    require(cursor == start + count, "invalid native hunk range");
  }
  while (cursor < lines.size())
    append(lines[cursor++]);
  return result;
}
Json publish(const Store &store, HunkPlan &plan, std::string_view chosen, OperationOptions options,
             bool verify_working) {
  require_native_branch_available(store, plan.branch);
  auto ingestion = store.settings().ingest_options();
  ingestion.stop = options.stop;
  ingestion.progress = [&](const ProgressEvent &event) {
    auto contextual = event;
    contextual.path = plan.path;
    report_progress(options, contextual);
  };
  std::istringstream input{std::string(chosen)};
  const auto manifest = store.ingest(input, {}, ingestion).first;
  auto snapshot = std::move(plan.snapshot);
  snapshot["parents"] = plan.head ? Json::array({plan.head->id}) : Json::array();
  snapshot["message"] = "Syrnck staging v1: " + plan.branch;
  snapshot["timestamp"] = 0;
  snapshot["files"][plan.path]["manifest"] = manifest;
  throw_if_cancelled(options.stop);
  const auto candidate = store.put(Kind::revision, bytes(snapshot.dump())).first;
  if (verify_working)
    verify_native_working_diff(store, plan.working, options);
  const auto selection = native_branch_lookup(plan.refs, store.native_workspace_reference("workspace/HEAD"));
  std::vector<RefUpdate> updates{{native_revision_reference(store, plan.branch), plan.head, plan.head},
                                 {native_staging_reference(store, plan.branch), plan.index, candidate},
                                 {store.native_workspace_reference("workspace/HEAD"), selection, selection}};
  if (plan.index)
    updates.push_back({plan.archive, native_branch_lookup(plan.refs, plan.archive), plan.index});
  plan.report["state"] = "applied";
  plan.report["previous_index"] = nullable(plan.index);
  plan.report["index"] = candidate.id;
  plan.report["result_manifest"] = manifest;
  plan.report["result_file_hash"] = hash(bytes(chosen));
  require(plan.report.dump().size() <= max_object_size, "native text selection plan exceeds 16 MiB");
  auto publication = options;
  publication.progress = [&](const ProgressEvent &event) {
    report_progress(options, event);
    if (verify_working && event.phase == ProgressPhase::publishing && event.path == "refs")
      verify_native_working_diff(store, plan.working, {options.stop});
  };
  require(store.compare_update_refs(updates, publication),
          "native staging references changed; retry operation");
  return std::move(plan.report);
}
Json apply(const Store &store, const fs::path *directory, std::string_view branch, std::string_view path,
           std::string_view expected, const std::vector<std::size_t> &hunks, TextDiffOptions limits,
           OperationOptions options, bool by_line = false) {
  throw_if_cancelled(options.stop);
  validate_id(expected);
  require(!hunks.empty(), "native text selection must not be empty");
  require(hunks.size() <= 100000, "native text selection exceeds 100000 numbers");
  std::set<std::size_t> selected(hunks.begin(), hunks.end());
  require(selected.size() == hunks.size() && !selected.contains(0),
          "native text selection numbers must be unique and positive");
  auto plan = prepare(store, directory, branch, path, limits, options, by_line);
  require(plan.report["plan_id"].get<std::string>() == expected,
          "native text selection plan changed; refresh the plan");
  if (by_line && directory)
    require(plan.report["ready"].get<bool>(),
            "native line staging is blocked; inspect native add-lines-plan");
  else if (by_line)
    require(plan.report["ready"].get<bool>(),
            "native line reset is blocked; inspect native reset-lines-plan");
  else if (directory)
    require(plan.report["ready"].get<bool>(),
            "native hunk staging is blocked; inspect native add-hunks-plan");
  else
    require(plan.report["ready"].get<bool>(),
            "native hunk reset is blocked; inspect native reset-hunks-plan");
  const auto count =
      by_line ? plan.report["change_count"].get<std::size_t>() : plan.report["diff"]["hunks"].size();
  require(*selected.rbegin() <= count, "native text selection number is out of range");
  if (!directory) {
    std::set<std::size_t> retained;
    for (std::size_t n = 1; n <= count; ++n)
      if (!selected.contains(n))
        retained.insert(n);
    selected = std::move(retained);
  }
  const auto chosen = select(plan, selected, limits, options, by_line);
  plan.report["selected"] = hunks;
  return publish(store, plan, chosen, options, directory != nullptr);
}
struct PatchPlan {
  HunkPlan state;
  TextPatchResult result;
};
PatchPlan prepare_patch(const Store &store, std::string_view branch, std::string_view path,
                        std::span<const std::uint8_t> patch, TextDiffOptions limits,
                        OperationOptions options) {
  limits.validate();
  require(patch.size() <= limits.max_output_bytes, "text patch input exceeds byte budget");
  PatchPlan plan{state(store, branch, path, options), {}};
  const auto source = read_native_stored_text(
      store, plan.state.source ? std::optional{plan.state.source->id} : std::nullopt, path, limits, options);
  plan.result = apply_text_patch(bytes(source.content), patch, path, limits, options);
  const auto result_hash = hash(bytes(plan.result.content));
  Json blockers = Json::array();
  if (store.checkout_status()["pending"].get<bool>())
    blockers.push_back(Json{{"reason", "pending_checkout"}});
  if (plan.state.head != plan.state.base)
    blockers.push_back(Json{{"reason", "stale_staging"}});
  if (result_hash == source.entry["file_hash"].get<std::string>())
    blockers.push_back(Json{{"reason", "no_text_changes"}});
  if (plan.state.index) {
    const auto archive = native_branch_lookup(plan.state.refs, plan.state.archive);
    if (archive && archive != plan.state.index)
      blockers.push_back(Json{{"reason", "archive_conflict"}, {"reference", plan.state.archive}});
  }
  auto &report = plan.state.report;
  report = Json{{"version", 1},
                {"operation", "stage-patch"},
                {"branch", branch},
                {"path", path},
                {"head", nullable(plan.state.head)},
                {"index", nullable(plan.state.index)},
                {"base", nullable(plan.state.base)},
                {"stale", plan.state.head != plan.state.base},
                {"source_revision", nullable(plan.state.source)},
                {"source", source.entry},
                {"patch_bytes", patch.size()},
                {"patch_hash", hash(patch)},
                {"result", Json{{"size", plan.result.content.size()},
                                {"file_hash", result_hash},
                                {"hunks", plan.result.hunks},
                                {"additions", plan.result.additions},
                                {"deletions", plan.result.deletions}}},
                {"objects_verified", true},
                {"executable_preserved", true},
                {"archive", plan.state.index ? Json(plan.state.archive) : Json{}},
                {"text_options", Json{{"context", limits.context},
                                      {"max_file_bytes", limits.max_file_bytes},
                                      {"max_lines", limits.max_lines},
                                      {"max_work", limits.max_work},
                                      {"max_output_bytes", limits.max_output_bytes}}},
                {"ready", blockers.empty()},
                {"blockers", std::move(blockers)}};
  const auto encoded = report.dump();
  require(encoded.size() + 100 <= max_object_size, "native text selection plan exceeds 16 MiB");
  report["plan_id"] = object_id(Kind::chunk, bytes(encoded));
  return plan;
}
} // namespace
Json Store::plan_stage_hunks(const fs::path &directory, std::string_view branch, std::string_view path,
                             TextDiffOptions limits, OperationOptions options) const {
  return prepare(*this, &directory, branch, path, limits, options).report;
}
Json Store::stage_hunks(const fs::path &directory, std::string_view branch, std::string_view path,
                        std::string_view expected, const std::vector<std::size_t> &hunks,
                        TextDiffOptions limits, OperationOptions options) const {
  return apply(*this, &directory, branch, path, expected, hunks, limits, options);
}
Json Store::plan_reset_hunks(std::string_view branch, std::string_view path, TextDiffOptions limits,
                             OperationOptions options) const {
  return prepare(*this, nullptr, branch, path, limits, options).report;
}
Json Store::reset_hunks(std::string_view branch, std::string_view path, std::string_view expected,
                        const std::vector<std::size_t> &hunks, TextDiffOptions limits,
                        OperationOptions options) const {
  return apply(*this, nullptr, branch, path, expected, hunks, limits, options);
}
Json Store::plan_stage_lines(const fs::path &directory, std::string_view branch, std::string_view path,
                             TextDiffOptions limits, OperationOptions options) const {
  return prepare(*this, &directory, branch, path, limits, options, true).report;
}
Json Store::stage_lines(const fs::path &directory, std::string_view branch, std::string_view path,
                        std::string_view expected, const std::vector<std::size_t> &lines,
                        TextDiffOptions limits, OperationOptions options) const {
  return apply(*this, &directory, branch, path, expected, lines, limits, options, true);
}
Json Store::plan_reset_lines(std::string_view branch, std::string_view path, TextDiffOptions limits,
                             OperationOptions options) const {
  return prepare(*this, nullptr, branch, path, limits, options, true).report;
}
Json Store::reset_lines(std::string_view branch, std::string_view path, std::string_view expected,
                        const std::vector<std::size_t> &lines, TextDiffOptions limits,
                        OperationOptions options) const {
  return apply(*this, nullptr, branch, path, expected, lines, limits, options, true);
}
Json Store::plan_stage_patch(std::string_view branch, std::string_view path,
                             std::span<const std::uint8_t> patch, TextDiffOptions limits,
                             OperationOptions options) const {
  return prepare_patch(*this, branch, path, patch, limits, options).state.report;
}
Json Store::stage_patch(std::string_view branch, std::string_view path, std::span<const std::uint8_t> patch,
                        std::string_view expected, TextDiffOptions limits, OperationOptions options) const {
  throw_if_cancelled(options.stop);
  validate_id(expected);
  auto plan = prepare_patch(*this, branch, path, patch, limits, options);
  require(plan.state.report["plan_id"].get<std::string>() == expected,
          "native text selection plan changed; refresh the plan");
  require(plan.state.report["ready"].get<bool>(),
          "native patch staging is blocked; inspect native add-patch-plan");
  return publish(*this, plan.state, plan.result.content, options, false);
}
} // namespace syrnck
