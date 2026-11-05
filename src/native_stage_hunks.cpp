#include "native_branch.hpp"
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
  std::string branch, path, archive;
  std::optional<ObjectKey> head, index, base, source;
  Json snapshot, report;
  NativeWorkingDiff working;
};
HunkPlan prepare(const Store &store, const fs::path &directory, std::string_view branch,
                 std::string_view path, TextDiffOptions limits, OperationOptions options) {
  throw_if_cancelled(options.stop);
  limits.validate();
  native_branch_name(branch);
  validate_path(path);
  HunkPlan plan;
  plan.refs = store.refs();
  plan.branch = branch;
  plan.path = path;
  plan.head = native_branch_lookup(plan.refs, "heads/" + plan.branch);
  plan.index = native_branch_lookup(plan.refs, "staging/" + plan.branch);
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
  plan.working = read_native_working_diff(
      store, directory, plan.source ? std::optional{plan.source->id} : std::nullopt, path, limits, options);
  auto &report = plan.working.report;
  Json blockers = Json::array();
  if (store.checkout_status()["pending"].get<bool>())
    blockers.push_back(Json{{"reason", "pending_checkout"}});
  if (plan.head != plan.base)
    blockers.push_back(Json{{"reason", "stale_staging"}});
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
  std::size_t number = 0;
  for (auto &hunk : report["diff"]["hunks"])
    hunk["number"] = ++number;
  report["operation"] = "stage-hunks";
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
  require(encoded.size() + 100 <= max_object_size, "native hunk plan exceeds 16 MiB");
  report["plan_id"] = object_id(Kind::chunk, bytes(encoded));
  plan.report = std::move(report);
  return plan;
}
std::string select(const HunkPlan &plan, const std::set<std::size_t> &selected, TextDiffOptions limits,
                   OperationOptions options) {
  std::vector<std::string_view> lines;
  const std::string_view source(plan.working.source);
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
    require(text.size() <= limits.max_file_bytes - result.size(), "native hunk result exceeds file budget");
    result += text;
  };
  for (const auto &hunk : plan.report["diff"]["hunks"]) {
    if (!selected.contains(hunk["number"].get<std::size_t>()))
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
                "native hunk source changed; refresh the plan");
        ++cursor;
      }
      if (line["kind"] != "remove")
        append(text);
    }
    require(cursor == start + count, "invalid native hunk range");
  }
  while (cursor < lines.size())
    append(lines[cursor++]);
  return result;
}
} // namespace
Json Store::plan_stage_hunks(const fs::path &directory, std::string_view branch, std::string_view path,
                             TextDiffOptions limits, OperationOptions options) const {
  return prepare(*this, directory, branch, path, limits, options).report;
}
Json Store::stage_hunks(const fs::path &directory, std::string_view branch, std::string_view path,
                        std::string_view expected, const std::vector<std::size_t> &hunks,
                        TextDiffOptions limits, OperationOptions options) const {
  throw_if_cancelled(options.stop);
  validate_id(expected);
  require(!hunks.empty(), "native hunk selection must not be empty");
  require(hunks.size() <= 100000, "native hunk selection exceeds 100000 numbers");
  const std::set<std::size_t> selected(hunks.begin(), hunks.end());
  require(selected.size() == hunks.size() && !selected.contains(0),
          "native hunk numbers must be unique and positive");
  auto plan = prepare(*this, directory, branch, path, limits, options);
  require(plan.report["plan_id"].get<std::string>() == expected,
          "native hunk plan changed; refresh the plan");
  require(plan.report["ready"].get<bool>(), "native hunk staging is blocked; inspect native add-hunks-plan");
  require(*selected.rbegin() <= plan.report["diff"]["hunks"].size(), "native hunk number is out of range");
  const auto chosen = select(plan, selected, limits, options);
  auto ingestion = settings().ingest_options();
  ingestion.stop = options.stop;
  ingestion.progress = [&](const ProgressEvent &event) {
    auto contextual = event;
    contextual.path = path;
    report_progress(options, contextual);
  };
  std::istringstream input(chosen);
  const auto manifest = ingest(input, {}, ingestion).first;
  auto snapshot = std::move(plan.snapshot);
  snapshot["parents"] = plan.head ? Json::array({plan.head->id}) : Json::array();
  snapshot["message"] = "Syrnck staging v1: " + plan.branch;
  snapshot["timestamp"] = 0;
  snapshot["files"][plan.path]["manifest"] = manifest;
  throw_if_cancelled(options.stop);
  const auto candidate = put(Kind::revision, bytes(snapshot.dump())).first;
  verify_native_working_diff(*this, plan.working, options);
  const auto selection = native_branch_lookup(plan.refs, "workspace/HEAD");
  std::vector<RefUpdate> updates{{"heads/" + plan.branch, plan.head, plan.head},
                                 {"staging/" + plan.branch, plan.index, candidate},
                                 {"workspace/HEAD", selection, selection}};
  if (plan.index)
    updates.push_back({plan.archive, native_branch_lookup(plan.refs, plan.archive), plan.index});
  plan.report["state"] = "applied";
  plan.report["previous_index"] = nullable(plan.index);
  plan.report["index"] = candidate.id;
  plan.report["selected"] = hunks;
  plan.report["result_manifest"] = manifest;
  plan.report["result_file_hash"] = hash(bytes(chosen));
  require(plan.report.dump().size() <= max_object_size, "native hunk plan exceeds 16 MiB");
  auto publication = options;
  publication.progress = [&](const ProgressEvent &event) {
    report_progress(options, event);
    if (event.phase == ProgressPhase::publishing && event.path == "refs")
      verify_native_working_diff(*this, plan.working, {options.stop});
  };
  require(compare_update_refs(updates, publication), "native staging references changed; retry operation");
  return std::move(plan.report);
}
} // namespace syrnck
