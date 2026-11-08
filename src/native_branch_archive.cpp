#include "native_branch_archive.hpp"
#include "native_context.hpp"
namespace syrnck {
namespace {
NativeBranchArchive archive_for(const NativeBranchState &branch) {
  NativeBranchArchive archive{{}, branch.name, branch.target, branch.index};
  const auto encoded = archive.json().dump();
  archive.id = object_id(Kind::chunk, bytes(encoded));
  return archive;
}
Json summary(const Store &store, const NativeBranchArchive &archive, OperationOptions options) {
  auto row = archive.state(store, options).json(false);
  row.erase("current");
  row["archive"] = archive.id;
  return row;
}
struct DeletePlan {
  NativeBranchArchive archive;
  Json report;
};
DeletePlan prepare_delete(const Store &store, const Refs &refs, const NativeBranchState &branch,
                          bool archive_index, bool verify, OperationOptions options) {
  const auto head = store.native_head();
  const bool current = head["branch"] == branch.name;
  auto archive = archive_for(branch);
  const auto state = archive.root_state(refs);
  Json blockers = Json::array();
  if (store.checkout_status()["pending"].get<bool>())
    blockers.push_back(Json{{"reason", "pending_checkout"}});
  if (current)
    blockers.push_back(Json{{"reason", "selected_branch"}});
  const auto owner = native_branch_workspace_owner(store, branch.name);
  if (owner && *owner != store.native_workspace_name())
    blockers.push_back(Json{{"reason", "branch_in_other_workspace"}, {"workspace", *owner}});
  if (branch.index && !archive_index)
    blockers.push_back(Json{{"reason", "staging_present"}});
  if (state < 0)
    blockers.push_back(Json{{"reason", "archive_conflict"}});
  if (state == 1)
    require(text(store.get({Kind::chunk, archive.id})) == archive.json().dump(),
            "invalid native branch archive");
  if (verify) {
    std::vector<ObjectKey> roots{branch.target};
    if (branch.index)
      roots.push_back(*branch.index);
    (void)store.closure(roots, true, options.stop, options.progress);
  }
  Json report{{"version", 1},
              {"operation", "branch-delete"},
              {"branch", branch.json(current)},
              {"archive", archive.id},
              {"archive_existing", state == 1},
              {"archive_index", archive_index},
              {"ready", blockers.empty()},
              {"objects_verified", verify},
              {"blockers", std::move(blockers)}};
  require(report.dump().size() <= max_object_size, "native branch report exceeds 16 MiB");
  report_progress(options, {ProgressPhase::planning, branch.name, 1, 1, true});
  return {std::move(archive), std::move(report)};
}
} // namespace
NativeBranchArchive read_native_branch_archive(const Store &store, const Refs &refs, std::string_view id,
                                               OperationOptions options) {
  throw_if_cancelled(options.stop);
  validate_id(id);
  NativeBranchArchive archive;
  archive.id = id;
  require(native_branch_lookup(refs, archive.prefix() + "/record") ==
              std::optional{ObjectKey{Kind::chunk, std::string(id)}},
          "unknown native branch archive");
  const auto data = store.get({Kind::chunk, std::string(id)});
  const auto value = parse_json(data);
  require(value.is_object() && value.size() == 5 && value.contains("version") &&
              value["version"].is_number_integer() && value["version"] == 1 && value.contains("type") &&
              value["type"] == "native-branch-archive" && value.contains("name") &&
              value["name"].is_string() && value.contains("revision") && value["revision"].is_string() &&
              value.contains("index") && (value["index"].is_null() || value["index"].is_string()),
          "invalid native branch archive");
  archive.name = value["name"].get<std::string>();
  native_branch_name(archive.name);
  archive.target.id = value["revision"].get<std::string>();
  validate_id(archive.target.id);
  if (!value["index"].is_null()) {
    archive.index = ObjectKey{Kind::revision, value["index"].get<std::string>()};
    validate_id(archive.index->id);
  }
  require(archive.json().dump() == text(data) && archive.root_state(refs) == 1,
          "invalid native branch archive");
  return archive;
}
Json Store::plan_delete_native_branch(std::string_view name, bool archive_index, bool verify_objects,
                                      OperationOptions options) const {
  throw_if_cancelled(options.stop);
  const auto all = refs();
  const NativeBranchState branch(*this, all, name, options);
  return prepare_delete(*this, all, branch, archive_index, verify_objects, options).report;
}
Json Store::delete_native_branch(std::string_view name, std::string_view expected,
                                 const std::optional<std::string> &expected_index,
                                 OperationOptions options) const {
  throw_if_cancelled(options.stop);
  validate_id(expected);
  if (expected_index)
    validate_id(*expected_index);
  const auto all = refs();
  const NativeBranchState branch(*this, all, name, options);
  require(branch.target.id == expected, "native branch tip changed; refresh the plan");
  require(branch.index ==
              (expected_index ? std::optional{ObjectKey{Kind::revision, *expected_index}} : std::nullopt),
          "native branch staging changed; refresh the plan");
  auto plan = prepare_delete(*this, all, branch, expected_index.has_value(), true, options);
  require(plan.report["ready"].get<bool>(),
          "native branch deletion is blocked; inspect native branch-delete-plan");
  throw_if_cancelled(options.stop);
  const auto record = put(Kind::chunk, bytes(plan.archive.json().dump())).first;
  require(record.id == plan.archive.id, "invalid native branch archive");
  const auto head = native_branch_lookup(all, this->native_workspace_reference("workspace/HEAD"));
  auto updates = plan.archive.retention_updates(all);
  updates.push_back({branch.ref, branch.target, {}});
  updates.push_back({branch.index_ref, branch.index, {}});
  updates.push_back({this->native_workspace_reference("workspace/HEAD"), head, head});
  require(compare_update_refs(updates, options), "native branch references changed; retry operation");
  plan.report["state"] = "deleted";
  return plan.report;
}
Json Store::native_branch_archive(std::string_view id, bool verify_objects, OperationOptions options) const {
  throw_if_cancelled(options.stop);
  const auto all = refs();
  const auto archive = read_native_branch_archive(*this, all, id, options);
  auto row = summary(*this, archive, options);
  if (verify_objects) {
    const auto retained = archive.roots();
    std::vector<ObjectKey> roots;
    for (const auto &[name, key] : retained) {
      (void)name;
      roots.push_back(key);
    }
    (void)closure(roots, true, options.stop, options.progress);
  }
  report_progress(options, {ProgressPhase::planning, {}, 1, 1, true});
  return Json{{"version", 1}, {"archive", std::move(row)}, {"objects_verified", verify_objects}};
}
Json Store::native_branch_archives(const std::optional<std::string> &after, std::size_t limit,
                                   bool verify_objects, OperationOptions options) const {
  throw_if_cancelled(options.stop);
  require(limit >= 1 && limit <= 1000, "native branch page limit must be in 1..1000");
  if (after)
    validate_id(*after);
  const auto all = refs();
  const std::string prefix(native_branch_archive_prefix);
  // A valid ID is exactly 64 characters. The suffix sorts after its '/' group.
  auto it = all.lower_bound(after ? prefix + *after + '0' : prefix);
  Json rows = Json::array();
  std::vector<ObjectKey> roots;
  std::size_t budget = 4096;
  for (; it != all.end() && it->first.starts_with(prefix) && rows.size() < limit;) {
    throw_if_cancelled(options.stop);
    const auto rest = it->first.substr(prefix.size());
    const auto slash = rest.find('/');
    require(slash != rest.npos, "invalid native branch archive");
    const auto id = rest.substr(0, slash);
    const auto archive = read_native_branch_archive(*this, all, id, options);
    auto row = summary(*this, archive, options);
    const auto size = row.dump().size() + 1;
    require(size <= max_object_size - budget, "native branch report exceeds 16 MiB");
    budget += size;
    rows.push_back(std::move(row));
    for (const auto &[name, key] : archive.roots()) {
      (void)name;
      roots.push_back(key);
    }
    const auto group = archive.prefix() + '/';
    do {
      ++it;
    } while (it != all.end() && it->first.starts_with(group));
    report_progress(options, {ProgressPhase::planning, id, rows.size()});
  }
  if (verify_objects)
    (void)closure(roots, true, options.stop, options.progress);
  const bool more = it != all.end() && it->first.starts_with(prefix);
  const auto next = more && !rows.empty() ? rows.back()["archive"] : Json{};
  report_progress(options, {ProgressPhase::planning, {}, rows.size(), rows.size(), true});
  return Json{{"version", 1},
              {"archives", std::move(rows)},
              {"has_more", more},
              {"next_after", next},
              {"objects_verified", verify_objects}};
}
} // namespace syrnck
