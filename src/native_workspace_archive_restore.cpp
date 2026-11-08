#include "native_workspace_archive.hpp"
#include <syrnck/workspace.hpp>
#include <algorithm>
namespace syrnck {
namespace {
std::optional<ObjectKey> lookup(const Refs &refs, const std::string &name) {
  const auto at = refs.find(name);
  return at == refs.end() ? std::optional<ObjectKey>{} : std::optional{at->second};
}
bool below(const fs::path &path, const fs::path &parent) {
  auto folded = [](const fs::path &value) {
    auto result = utf8(value.lexically_normal());
    std::replace(result.begin(), result.end(), '\\', '/');
    while (result.size() > 1 && result.back() == '/') result.pop_back();
    return portable_fold(result);
  };
  const auto child = folded(path), root = folded(parent);
  return child == root || (child.starts_with(root) && (root.back() == '/' || child[root.size()] == '/'));
}
ObjectKey chunk_key(const Json &value) { return {Kind::chunk, object_id(Kind::chunk, bytes(value.dump()))}; }
struct Plan {
  NativeWorkspaceArchive archive;
  fs::path primary, directory;
  std::string name, target;
  Json registration, head, index, primary_head, report;
  ObjectKey registration_key{Kind::chunk, {}}, head_key{Kind::chunk, {}}, primary_key{Kind::chunk, {}};
  std::optional<ObjectKey> index_key, policy;
  std::vector<RefUpdate> updates;
  bool existing = false, registered = false;
};
Plan prepare(const Store &store, const fs::path &primary, std::string_view id, std::string_view name,
    const fs::path &directory, NativeWorkspaceRestoreOptions settings, bool verify, OperationOptions options) {
  throw_if_cancelled(options.stop);
  require(store.native_workspace_name().empty(), "workspace restoration requires the primary context");
  require_no_native_checkout(store);
  validate_native_workspace_name(name);
  require(!(settings.branch && settings.detached), "choose a named or detached workspace restoration");
  store.validate_native_workspace(primary);
  Plan plan;
  plan.name = name;
  plan.primary = fs::canonical(primary);
  const auto selected = store.native_head();
  require(selected["directory"].is_null() || selected["directory"] == utf8(plan.primary),
      "native HEAD belongs to another workspace");
  const auto refs = store.refs();
  plan.archive = read_native_workspace_archive(store, refs, id, options);
  const auto original = plan.archive.branch.value_or(std::string(detached_native_target));
  plan.target = settings.detached ? std::string(detached_native_target) : settings.branch.value_or(original);
  validate_native_target(plan.target);
  if (settings.branch) validate_ref(*settings.branch);
  plan.directory = fs::absolute(directory).lexically_normal();
  require(plan.directory != plan.directory.root_path(), "native workspaces must not overlap");
  const auto status = fs::symlink_status(plan.directory);
  plan.existing = status.type() != fs::file_type::not_found;
  if (plan.existing)
    require(fs::is_directory(status) && !native_link_like(plan.directory, status), "invalid native workspace directory");
  plan.directory = plan.existing ? fs::canonical(plan.directory)
      : fs::canonical(plan.directory.parent_path()) / plan.directory.filename();
  require(!below(plan.directory, plan.primary) && !below(plan.primary, plan.directory) &&
              !below(plan.directory, store.root()) && !below(store.root(), plan.directory),
      "native workspaces must not overlap");
  const auto registration_name = native_context_reference(name, "registration"), head_name = native_context_reference(name, "workspace/HEAD");
  plan.registration = Json{{"version", 1}, {"type", "native-workspace"}, {"name", name}, {"directory", utf8(plan.directory)}};
  plan.registration_key = chunk_key(plan.registration);
  plan.head = Json{{"version", native_target_is_detached(plan.target) ? 2 : 1}, {"type", "native-head"},
      {"directory", utf8(plan.directory)}, {"branch", native_target_is_detached(plan.target) ? Json{} : Json(plan.target)}};
  plan.head_key = chunk_key(plan.head);
  if (plan.archive.index) {
    plan.index = store.metadata(Kind::revision, *plan.archive.index);
    require(plan.index["message"] == "Syrnck staging v1: " + original && plan.index["timestamp"] == 0 &&
                plan.index["parents"].size() <= 1, "invalid native workspace archive");
    plan.index["message"] = "Syrnck staging v1: " + plan.target;
    validate_object(Kind::revision, bytes(plan.index.dump()));
    plan.index_key = ObjectKey{Kind::revision, object_id(Kind::revision, bytes(plan.index.dump()))};
  }
  if (!plan.archive.materialization.complete())
    plan.policy = ObjectKey{Kind::chunk, plan.archive.materialization.identity()};
  plan.registered = lookup(refs, registration_name).has_value();
  const auto prefix = "workspaces/" + std::string(name) + '/';
  const auto first = refs.lower_bound(prefix);
  require(plan.registered || first == refs.end() || !first->first.starts_with(prefix), "native workspace name already exists");
  const auto index_name = native_staging_reference(name, plan.target);
  const auto target_name = native_target_is_detached(plan.target)
      ? native_context_reference(name, "workspace/detached") : native_revision_reference(plan.target);
  const auto policy_name = native_context_reference(name, "workspace/materialization");
  const auto restored_name = native_context_reference(name, "restored/" + std::string(id));
  const ObjectKey working{Kind::revision, plan.archive.work};
  const ObjectKey revision{Kind::revision, plan.archive.head};
  if (plan.registered) {
    require(lookup(refs, registration_name) == plan.registration_key && lookup(refs, head_name) == plan.head_key &&
                lookup(refs, index_name) == plan.index_key && lookup(refs, target_name) == revision &&
                lookup(refs, policy_name) == plan.policy && lookup(refs, restored_name) == working && plan.existing,
        "native workspace name already exists");
  }
  const auto rows = store.native_workspaces(options)["workspaces"];
  std::size_t count = 0;
  for (const auto &row : rows) {
    if (row["name"] == "primary") continue;
    ++count;
    if (row["name"] == Json(name) && plan.registered) continue;
    const auto other = path_from_utf8(row["directory"].get<std::string>());
    require(!below(plan.directory, other) && !below(other, plan.directory), "native workspaces must not overlap");
  }
  require(plan.registered || count < 1024, "native workspace registration limit exceeded");
  std::optional<ObjectKey> branch_tip;
  if (!native_target_is_detached(plan.target)) {
    const auto owner = native_branch_workspace_owner(store, plan.target);
    require(!owner || (*owner == name && plan.registered), "native branch is selected in another workspace");
    branch_tip = lookup(refs, native_revision_reference(plan.target));
    require(!branch_tip || branch_tip == revision, "archived workspace branch changed; choose another branch or detach");
    require(lookup(refs, "workspace/HEAD") || plan.target != "main", "native branch is selected in another workspace");
  }
  for (const auto &[root, key] : plan.archive.roots()) plan.updates.push_back({root, key, key});
  plan.updates.push_back({registration_name, plan.registered ? std::optional{plan.registration_key} : std::nullopt, plan.registration_key});
  plan.updates.push_back({head_name, plan.registered ? std::optional{plan.head_key} : std::nullopt, plan.head_key});
  plan.updates.push_back({target_name, native_target_is_detached(plan.target)
      ? (plan.registered ? std::optional{revision} : std::nullopt) : branch_tip, revision});
  plan.updates.push_back({index_name, plan.registered ? plan.index_key : std::nullopt, plan.index_key});
  plan.updates.push_back({policy_name, plan.registered ? plan.policy : std::nullopt, plan.policy});
  plan.updates.push_back({restored_name, plan.registered ? std::optional{working} : std::nullopt, working});
  const auto primary_key = lookup(refs, "workspace/HEAD");
  if (primary_key) plan.primary_key = *primary_key;
  else {
    plan.primary_head = Json{{"version", 1}, {"type", "native-head"}, {"directory", utf8(plan.primary)}, {"branch", "main"}};
    plan.primary_key = chunk_key(plan.primary_head);
  }
  plan.updates.push_back({"workspace/HEAD", primary_key, plan.primary_key});
  auto prospective = refs;
  for (const auto &update : plan.updates) {
    if (update.value) prospective[update.name] = *update.value;
    else prospective.erase(update.name);
  }
  require(encode_refs(prospective).dump().size() <= max_object_size, "reference document exceeds 16 MiB");
  if (verify) verify_native_workspace_archive(store, plan.archive, options);
  if (plan.existing) verify_native_restored_directory(store, plan.directory, plan.archive.work, options);
  const Json identity{{"version", 1}, {"type", "native-workspace-archive-restore-plan"},
      {"store", utf8(store.root())}, {"primary", utf8(plan.primary)}, {"archive", id}, {"name", name},
      {"directory", utf8(plan.directory)}, {"target", plan.target}, {"revision", plan.archive.head},
      {"work", plan.archive.work}, {"index", plan.index_key ? Json(plan.index_key->id) : Json{}},
      {"materialization", plan.archive.materialization.definition()}};
  plan.report = Json{{"version", 1}, {"operation", "workspace-archive-restore"}, {"archive", id},
      {"name", name}, {"directory", utf8(plan.directory)},
      {"branch", native_target_is_detached(plan.target) ? Json{} : Json(plan.target)},
      {"revision", plan.archive.head}, {"work", plan.archive.work},
      {"index", plan.index_key ? Json(plan.index_key->id) : Json{}},
      {"materialization", plan.archive.materialization.definition()}, {"ready", true},
      {"objects_verified", verify}, {"reused_directory", plan.existing}, {"already_registered", plan.registered},
      {"plan", hash(bytes("Syrnck workspace archive restore plan v1:" + identity.dump()))}};
  require(plan.report.dump().size() <= max_object_size, "native workspace archive report exceeds 16 MiB");
  return plan;
}
}
Json plan_native_workspace_archive_restore(const Store &store, const fs::path &primary,
    std::string_view archive, std::string_view name, const fs::path &directory,
    NativeWorkspaceRestoreOptions settings, bool verify, OperationOptions options) {
  return prepare(store, primary, archive, name, directory, settings, verify, options).report;
}
Json restore_native_workspace_archive(const Store &store, const fs::path &primary,
    std::string_view archive, std::string_view name, const fs::path &directory,
    std::string_view expected_plan, NativeWorkspaceRestoreOptions settings, OperationOptions options) {
  validate_id(expected_plan);
  auto plan = prepare(store, primary, archive, name, directory, settings, true, options);
  require(plan.report["plan"].get<std::string>() == expected_plan,
      "native workspace archive restore plan changed; inspect the plan and retry");
  if (plan.registered) {
    const auto latest = store.refs();
    for (const auto &update : plan.updates)
      require(lookup(latest, update.name) == update.value, "native workspace references changed; retry operation");
    verify_native_restored_directory(store, plan.directory, plan.archive.work, {options.stop});
    plan.report["state"] = "restored"; return plan.report;
  }
  store.require_features(plan.policy ? std::vector<std::string>{"linked-workspaces-v1", "sparse-workspaces-v1"}
                                    : std::vector<std::string>{"linked-workspaces-v1"}, options);
  store.put(Kind::chunk, bytes(plan.registration.dump()));
  store.put(Kind::chunk, bytes(plan.head.dump()));
  if (plan.index_key) store.put(Kind::revision, bytes(plan.index.dump()));
  if (plan.policy) store.put(Kind::chunk, bytes(plan.archive.materialization.encode()));
  if (!plan.primary_head.is_null()) store.put(Kind::chunk, bytes(plan.primary_head.dump()));
  if (!plan.existing) export_native_materialization(store, plan.archive.work, plan.directory, {}, options);
  report_progress(options, {ProgressPhase::publishing, "workspace-archive-restore"});
  const auto latest = store.refs();
  const auto prefix = "workspaces/" + std::string(name) + '/';
  const auto first = latest.lower_bound(prefix);
  require(first == latest.end() || !first->first.starts_with(prefix), "native workspace name already exists");
  verify_native_restored_directory(store, plan.directory, plan.archive.work, {options.stop});
  require(store.compare_update_refs(plan.updates, {options.stop}), "native workspace references changed; retry operation");
  plan.report["state"] = "restored";
  return plan.report;
}
} // namespace syrnck
