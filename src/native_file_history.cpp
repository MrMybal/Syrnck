#include "native_workspace.hpp"
#include <syrnck/history.hpp>
#include "native_authorship.hpp"
#include <algorithm>
namespace syrnck {
namespace {
using Identity = std::pair<std::uint64_t, std::string>;
Identity identity(const Json &file) {
  return {file["size"].get<std::uint64_t>(), file["file_hash"].get<std::string>()};
}
void budget(std::size_t &used, const Json &row) {
  const auto cost = row.dump().size() + 1;
  require(cost <= max_object_size - used, "native file history report exceeds 16 MiB");
  used += cost;
}
} // namespace
Json native_diff_renames(const Store &store, const std::optional<std::string> &source,
                         const std::optional<std::string> &target, OperationOptions options) {
  auto report = compare_native_snapshots(store, source, target, options);
  auto &changes = report["changes"];
  require(changes.size() <= max_object_size / 128, "native rename comparison exceeds path limit");
  struct Group {
    std::vector<std::size_t> removed, added;
  };
  std::map<Identity, Group> groups;
  for (std::size_t i = 0; i < changes.size(); ++i) {
    throw_if_cancelled(options.stop);
    const auto &row = changes[i];
    if (row["status"] == "removed")
      groups[identity(row["source"])].removed.push_back(i);
    else if (row["status"] == "added")
      groups[identity(row["target"])].added.push_back(i);
  }
  std::vector<bool> consumed(changes.size());
  Json renamed = Json::array();
  std::uint64_t ambiguous_sources = 0, ambiguous_targets = 0;
  std::size_t used = 4096;
  for (const auto &[key, group] : groups) {
    (void)key;
    throw_if_cancelled(options.stop);
    if (group.removed.size() == 1 && group.added.size() == 1) {
      const auto old = group.removed[0], next = group.added[0];
      auto row = changes[next];
      row["status"] = "renamed";
      row["old_path"] = changes[old]["path"];
      row["source"] = changes[old]["source"];
      row["content_changed"] = false;
      row["similarity"] = 100;
      row["executable_changed"] = row["source"]["executable"] != row["target"]["executable"];
      row["manifest_changed"] = row["source"]["manifest"] != row["target"]["manifest"];
      budget(used, row);
      renamed.push_back(std::move(row));
      consumed[old] = consumed[next] = true;
    } else if (!group.removed.empty() && !group.added.empty()) {
      ambiguous_sources += group.removed.size();
      ambiguous_targets += group.added.size();
      for (const auto index : group.removed)
        changes[index]["rename_ambiguous"] = true;
      for (const auto index : group.added)
        changes[index]["rename_ambiguous"] = true;
    }
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
  report["rename_detection"] = Json{
      {"mode", "exact"}, {"ambiguous_sources", ambiguous_sources}, {"ambiguous_targets", ambiguous_targets}};
  return report;
}
Json native_file_history(const Store &store, std::string_view revision, std::string_view path,
                         NativeFileHistoryOptions limits, OperationOptions options) {
  throw_if_cancelled(options.stop);
  validate_id(revision);
  validate_path(path);
  require(limits.limit >= 1 && limits.limit <= 1000, "native file history page limit must be in 1..1000");
  require(limits.max_revisions >= 1 && limits.max_revisions <= 10000,
          "native file history traversal limit must be in 1..10000");
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
    auto comparison = limits.follow_renames ? native_diff_renames(store, parent, current, options)
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
              {"entries", std::move(rows)},
              {"visited_revisions", seen.size()},
              {"complete", !current},
              {"stop_reason", stop_reason},
              {"next_revision", current ? Json(*current) : Json{}},
              {"next_path", current ? Json(selected) : Json{}}};
}
} // namespace syrnck
