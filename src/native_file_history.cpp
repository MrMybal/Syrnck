#include "native_workspace.hpp"
#include <syrnck/history.hpp>
#include "native_authorship.hpp"
#include "native_renames.hpp"
#include <algorithm>
namespace syrnck {
namespace {
void budget(std::size_t &used, const Json &row) {
  const auto cost = row.dump().size() + 1;
  require(cost <= max_object_size - used, "native file history report exceeds 16 MiB");
  used += cost;
}
Json diff_with_renames(const Store &store, const std::optional<std::string> &source,
                       const std::optional<std::string> &target, NativeRenameBatch *batch,
                       OperationOptions options) {
  auto report = compare_native_snapshots(store, source, target, options);
  auto &changes = report["changes"];
  require(changes.size() <= max_object_size / 128, "native rename comparison exceeds path limit");
  NativeSnapshot old_files, new_files;
  std::map<std::string, std::size_t> old_rows, new_rows;
  auto decode = [](const Json &file) {
    return NativeSnapshotFile{file["manifest"].get<std::string>(),
      {file["size"].get<std::uint64_t>(), file["file_hash"].get<std::string>()},
      file["executable"].get<bool>()};
  };
  for (std::size_t i = 0; i < changes.size(); ++i) {
    throw_if_cancelled(options.stop);
    auto &row = changes[i];
    const auto path = row["path"].get<std::string>();
    if (row["status"] == "removed") {
      old_files.emplace(path, decode(row["source"])); old_rows.emplace(path, i);
    } else if (row["status"] == "added") {
      new_files.emplace(path, decode(row["target"])); new_rows.emplace(path, i);
    }
  }
  const auto matches = match_native_renames(old_files, new_files, batch, options);
  for (const auto &path : matches.ambiguous_sources) changes[old_rows.at(path)]["rename_ambiguous"] = true;
  for (const auto &path : matches.ambiguous_targets) changes[new_rows.at(path)]["rename_ambiguous"] = true;
  std::vector<bool> consumed(changes.size());
  Json renamed = Json::array();
  std::size_t used = 4096;
  for (const auto &[old_path, match] : matches.matches) {
    throw_if_cancelled(options.stop);
    const auto old = old_rows.at(old_path), next = new_rows.at(match.target);
    auto row = changes[next];
    row["status"] = "renamed";
    row["old_path"] = old_path;
    row["source"] = changes[old]["source"];
    row["content_changed"] = old_files.at(old_path).content != new_files.at(match.target).content;
    row["similarity"] = match.similarity;
    row["executable_changed"] = row["source"]["executable"] != row["target"]["executable"];
    row["manifest_changed"] = row["source"]["manifest"] != row["target"]["manifest"];
    budget(used, row);
    renamed.push_back(std::move(row));
    consumed[old] = consumed[next] = true;
  }
  const auto count = renamed.size();
  for (std::size_t i = 0; i < changes.size(); ++i)
    if (!consumed[i]) {
      throw_if_cancelled(options.stop);
      budget(used, changes[i]);
      renamed.push_back(std::move(changes[i]));
    }
  std::sort(renamed.begin(), renamed.end(), [](const auto &a, const auto &b) {
    return a["path"].template get_ref<const std::string &>() <
           b["path"].template get_ref<const std::string &>();
  });
  report["changes"] = std::move(renamed);
  report["counts"]["added"] = report["counts"]["added"].get<std::uint64_t>() - count;
  report["counts"]["removed"] = report["counts"]["removed"].get<std::uint64_t>() - count;
  report["counts"]["renamed"] = count;
  report["rename_detection"] = matches.report;
  return report;
}
} // namespace
Json native_diff_renames(const Store &store, const std::optional<std::string> &source,
                         const std::optional<std::string> &target, OperationOptions options) {
  return diff_with_renames(store, source, target, nullptr, options);
}
Json native_diff_renames(const Store &store, const std::optional<std::string> &source,
                         const std::optional<std::string> &target, const NativeRenameOptions &limits,
                         OperationOptions options) {
  NativeRenameBatch batch(store, limits);
  return diff_with_renames(store, source, target, &batch, options);
}
Json native_file_history(const Store &store, std::string_view revision, std::string_view path,
                         NativeFileHistoryOptions limits, OperationOptions options) {
  throw_if_cancelled(options.stop);
  validate_id(revision);
  validate_path(path);
  require(limits.limit >= 1 && limits.limit <= 1000, "native file history page limit must be in 1..1000");
  require(limits.max_revisions >= 1 && limits.max_revisions <= 10000,
          "native file history traversal limit must be in 1..10000");
  require(limits.follow_renames || !limits.renames, "rename similarity requires rename following");
  std::optional<NativeRenameBatch> renames;
  if (limits.renames) renames.emplace(store, *limits.renames);
  std::optional<std::string> current{revision};
  std::string selected(path);
  std::set<std::string> seen;
  std::string stop_reason;
  Json rows = Json::array();
  std::size_t used = 4096;
  while (current && rows.size() < limits.limit && seen.size() < limits.max_revisions) {
    throw_if_cancelled(options.stop);
    require(seen.insert(*current).second, "native revision graph contains a cycle");
    const auto metadata = store.metadata(Kind::revision, *current);
    const auto parent = metadata["parents"].empty()
                            ? std::optional<std::string>{}
                            : std::optional{metadata["parents"][0].get<std::string>()};
    auto comparison = limits.follow_renames ? diff_with_renames(store, parent, current, renames ? &*renames : nullptr, options)
                                            : compare_native_snapshots(store, parent, current, options);
    const Json *change = nullptr;
    for (const auto &row : comparison["changes"])
      if (row["path"].get<std::string>() == selected) {
        change = &row;
        break;
      }
    if (!change && !metadata["files"].contains(selected) && limits.follow_renames)
      for (const auto &row : comparison["changes"])
        if (row["status"] == "renamed" && row["old_path"].get<std::string>() == selected) {
          change = &row;
          break;
        }
    bool created = false;
    if (change) {
      auto message = metadata["message"].get<std::string>();
      auto end = std::min<std::size_t>(256, message.size());
      if (end < message.size())
        while (end && (static_cast<unsigned char>(message[end]) & 0xc0) == 0x80)
          --end;
      Json row{{"revision", *current},
               {"path_at_revision", (*change)["path"]},
               {"timestamp", metadata["timestamp"]},
               {"parents", metadata["parents"]},
               {"message", message.substr(0, end)},
               {"message_truncated", end < message.size()},
               {"change", *change}};
      append_native_authorship(row, metadata);
      budget(used, row);
      rows.push_back(std::move(row));
      if ((*change)["status"] == "renamed")
        selected = (*change)["old_path"].get<std::string>();
      created = (*change)["status"] == "added";
      if (created)
        stop_reason = change->value("rename_ambiguous", false) ? "ambiguous_rename" : "added";
    }
    if (!parent && !created)
      stop_reason = "root";
    current = created ? std::optional<std::string>{} : parent;
    report_progress(options, {ProgressPhase::planning, selected, seen.size()});
  }
  report_progress(options, {ProgressPhase::planning, {}, seen.size(), seen.size(), true});
  if (current)
    stop_reason = rows.size() >= limits.limit ? "page_limit" : "traversal_limit";
  return Json{{"version", 1},
              {"revision", revision},
              {"requested_path", path},
              {"traversal", "first-parent"},
              {"follow_renames", limits.follow_renames},
              {"rename_detection", renames ? renames->report() : Json{{"mode", "exact"}}},
              {"entries", std::move(rows)},
              {"visited_revisions", seen.size()},
              {"complete", !current},
              {"stop_reason", stop_reason},
              {"next_revision", current ? Json(*current) : Json{}},
              {"next_path", current ? Json(selected) : Json{}}};
}
} // namespace syrnck
