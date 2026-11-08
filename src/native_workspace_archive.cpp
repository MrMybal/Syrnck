#include "native_workspace_archive.hpp"
#include "native_hard_reset.hpp"
#include <syrnck/workspace.hpp>
namespace syrnck {
namespace {
std::optional<ObjectKey> lookup(const Refs &refs, const std::string &name) {
  const auto at = refs.find(name);
  return at == refs.end() ? std::optional<ObjectKey>{} : std::optional{at->second};
}
Refs scoped(const Refs &refs, std::string_view name) {
  Refs result;
  const auto prefix = "workspaces/" + std::string(name) + '/';
  for (auto at = refs.lower_bound(prefix); at != refs.end() && at->first.starts_with(prefix); ++at)
    result.emplace(*at);
  return result;
}
// Historical bindings are evidence, never restoration destinations. Validate
// their generic spelling independently of this reader's native path grammar.
bool historical_directory(std::string_view value) {
  if (value.size() < 2 || value.back() == '/' || value.find('\0') != value.npos)
    return false;
  std::size_t begin = 0, minimum = 1;
  if (value.starts_with("//")) { begin = 2; minimum = 2; }
  else if (value.front() == '/') begin = 1;
  else if (value.size() > 3 && ((value[0] >= 'A' && value[0] <= 'Z') || (value[0] >= 'a' && value[0] <= 'z')) &&
      value[1] == ':' && value[2] == '/') begin = 3;
  else return false;
  std::size_t count = 0;
  while (begin < value.size()) {
    const auto end = value.find('/', begin);
    const auto part = value.substr(begin, end == value.npos ? value.size() - begin : end - begin);
    if (part.empty() || part == "." || part == "..") return false;
    ++count;
    if (end == value.npos) break;
    begin = end + 1;
  }
  return count >= minimum;
}
struct Plan {
  NativeWorkspaceArchive archive;
  NativeHardResetPlan capture;
  NativeWorkspaceContext context;
  Json report;
};
Plan prepare(const Store &store, bool include_untracked, bool verify, OperationOptions options) {
  throw_if_cancelled(options.stop);
  require(!store.native_workspace_name().empty(), "workspace unregistration requires a linked context");
  require_no_native_checkout(store);
  Plan plan;
  plan.archive.name = store.native_workspace_name();
  plan.context = read_native_workspace_context(store, plan.archive.name);
  const auto refs = store.refs();
  plan.archive.references = scoped(refs, plan.archive.name);
  const auto head = store.native_head();
  require(head["revision"].is_string(), "native workspace has no retained revision");
  if (!head["branch"].is_null()) plan.archive.branch = head["branch"].get<std::string>();
  plan.archive.head = head["revision"].get<std::string>();
  std::set<std::string> additional;
  if (include_untracked)
    visit_native_files(plan.context.directory, store,
        [&](const NativeFile &file) { additional.insert(portable_fold(file.relative)); }, options);
  plan.capture = prepare_native_hard_reset(store, plan.context.directory, store.native_target(),
      plan.archive.head, verify, options, additional);
  if (plan.capture.index) plan.archive.index = plan.capture.index->id;
  plan.archive.materialization = plan.capture.materialization.policy;
  plan.archive.include_untracked = include_untracked;
  const Json identity{{"version", 1}, {"type", "native-workspace-unregister-plan"},
      {"store", utf8(store.root())}, {"name", plan.archive.name},
      {"directory", utf8(plan.context.directory)}, {"references", encode_refs(plan.archive.references)},
      {"head", plan.archive.head}, {"capture", plan.capture.report["plan"]},
      {"include_untracked", include_untracked}};
  auto preview = plan.archive;
  preview.id = std::string(64, '0'); preview.nonce = std::string(64, '0');
  preview.work = std::string(64, '0'); preview.created_at = static_cast<std::uint64_t>(INT64_MAX);
  auto prospective = refs;
  for (const auto &[reference, key] : preview.references) { (void)key; prospective.erase(reference); }
  for (const auto &[reference, key] : preview.roots()) prospective[reference] = key;
  Json blockers = Json::array();
  if (preview.json().dump().size() > max_object_size || encode_refs(prospective).dump().size() > max_object_size)
    blockers.push_back(Json{{"reason", "reference_budget"}});
  plan.report = Json{{"version", 1}, {"operation", "workspace-unregister"},
      {"name", plan.archive.name}, {"directory", utf8(plan.context.directory)},
      {"revision", plan.archive.head}, {"index", plan.archive.index ? Json(*plan.archive.index) : Json{}},
      {"materialization", plan.archive.materialization.definition()},
      {"references", plan.archive.references.size()}, {"include_untracked", include_untracked},
      {"working", plan.capture.report["working"]}, {"directory_preserved", true},
      {"ready", blockers.empty() && plan.capture.report["ready"].get<bool>()},
      {"blockers", blockers}, {"objects_verified", verify},
      {"plan", hash(bytes("Syrnck workspace unregister plan v1:" + identity.dump()))}};
  require(plan.report.dump().size() <= max_object_size, "native workspace archive report exceeds 16 MiB");
  return plan;
}
}
Json NativeWorkspaceArchive::json() const {
  return Json{{"version", 1}, {"type", "native-workspace-archive"}, {"name", name}, {"nonce", nonce},
      {"branch", branch ? Json(*branch) : Json{}}, {"revision", head}, {"index", index ? Json(*index) : Json{}},
      {"work", work}, {"materialization", materialization.definition()},
      {"include_untracked", include_untracked}, {"created_at", created_at}, {"references", encode_refs(references)}};
}
Refs NativeWorkspaceArchive::roots() const {
  Refs result{{prefix() + "/record", {Kind::chunk, id}}, {prefix() + "/head", {Kind::revision, head}},
      {prefix() + "/work", {Kind::revision, work}}};
  for (const auto &[reference, key] : references)
    result.emplace(prefix() + "/roots/" + hash(bytes(reference)), key);
  return result;
}
NativeWorkspaceArchive read_native_workspace_archive(const Store &store, const Refs &refs,
    std::string_view id, OperationOptions options) {
  throw_if_cancelled(options.stop);
  validate_id(id);
  NativeWorkspaceArchive archive;
  archive.id = id;
  require(lookup(refs, archive.prefix() + "/record") == ObjectKey{Kind::chunk, std::string(id)},
      "unknown native workspace archive");
  const auto data = store.get({Kind::chunk, std::string(id)});
  const auto value = parse_json(data);
  require(value.is_object() && value.size() == 12 && value.value("version", 0) == 1 &&
              value.value("type", "") == "native-workspace-archive" && value.at("created_at").is_number_unsigned() &&
              value.at("include_untracked").is_boolean(), "invalid native workspace archive");
  archive.name = value.at("name").get<std::string>();
  validate_native_workspace_name(archive.name);
  archive.nonce = value.at("nonce").get<std::string>(); validate_id(archive.nonce);
  archive.head = value.at("revision").get<std::string>(); validate_id(archive.head);
  archive.work = value.at("work").get<std::string>(); validate_id(archive.work);
  archive.created_at = value.at("created_at").get<std::uint64_t>();
  require(archive.created_at <= static_cast<std::uint64_t>(INT64_MAX), "invalid native workspace archive");
  archive.include_untracked = value.at("include_untracked").get<bool>();
  if (!value.at("branch").is_null()) { archive.branch = value.at("branch").get<std::string>(); validate_ref(*archive.branch); }
  if (!value.at("index").is_null()) { archive.index = value.at("index").get<std::string>(); validate_id(*archive.index); }
  archive.materialization = NativeMaterialization::from_definition(value.at("materialization"));
  archive.references = parse_refs(bytes(value.at("references").dump()), "invalid native workspace archive");
  require(!archive.references.empty() && scoped(archive.references, archive.name) == archive.references,
      "invalid native workspace archive");
  const auto selection = lookup(archive.references, native_context_reference(archive.name, "workspace/HEAD"));
  const auto registration = lookup(archive.references, native_context_reference(archive.name, "registration"));
  require(selection && registration && selection->kind == Kind::chunk && registration->kind == Kind::chunk,
      "invalid native workspace archive");
  const auto binding = parse_json(store.get(*registration)), selected = parse_json(store.get(*selection));
  require(binding.is_object() && binding.size() == 4 && binding.value("version", 0) == 1 &&
              binding.value("type", "") == "native-workspace" && binding.at("name") == archive.name &&
              selected.is_object() && selected.size() == 4 && selected.value("type", "") == "native-head" &&
              selected.at("directory") == binding.at("directory") &&
              selected.at("branch") == (archive.branch ? Json(*archive.branch) : Json{}) &&
              selected.at("version") == (archive.branch ? 1 : 2), "invalid native workspace archive");
  const auto directory = binding.at("directory").get<std::string>();
  require(historical_directory(directory) &&
              binding.dump() == Json{{"version", 1}, {"type", "native-workspace"}, {"name", archive.name},
                  {"directory", directory}}.dump() &&
              selected.dump() == Json{{"version", archive.branch ? 1 : 2}, {"type", "native-head"},
                  {"directory", directory}, {"branch", archive.branch ? Json(*archive.branch) : Json{}}}.dump(),
      "invalid native workspace archive");
  const auto target = archive.branch.value_or(std::string(detached_native_target));
  require(lookup(archive.references, native_staging_reference(archive.name, target)) ==
              (archive.index ? std::optional{ObjectKey{Kind::revision, *archive.index}} : std::nullopt),
      "invalid native workspace archive");
  if (!archive.branch)
    require(lookup(archive.references, native_context_reference(archive.name, "workspace/detached")) ==
                ObjectKey{Kind::revision, archive.head}, "invalid native workspace archive");
  const auto policy = archive.materialization.complete() ? std::optional<ObjectKey>{}
      : std::optional{ObjectKey{Kind::chunk, archive.materialization.identity()}};
  require(lookup(archive.references, native_context_reference(archive.name, "workspace/materialization")) == policy,
      "invalid native workspace archive");
  const auto expected = archive.roots();
  Refs actual;
  for (auto at = refs.lower_bound(archive.prefix() + '/'); at != refs.end() && at->first.starts_with(archive.prefix() + '/'); ++at)
    actual.emplace(*at);
  require(actual == expected && archive.json().dump() == text(data), "invalid native workspace archive");
  const auto work = store.metadata(Kind::revision, archive.work);
  Json parents = Json::array({archive.head});
  if (archive.index && *archive.index != archive.head) parents.push_back(*archive.index);
  require(work["parents"] == parents && work["timestamp"] == 0 &&
              work["message"] == "Syrnck workspace archive work v1: " + archive.name,
      "invalid native workspace archive");
  if (archive.index) {
    const auto index = store.metadata(Kind::revision, *archive.index);
    require(index["message"] == "Syrnck staging v1: " + target && index["timestamp"] == 0 &&
                index["parents"].size() <= 1, "invalid native workspace archive");
  }
  return archive;
}
void verify_native_workspace_archive(const Store &store, const NativeWorkspaceArchive &archive, OperationOptions options) {
  std::vector<ObjectKey> roots;
  for (const auto &[name, key] : archive.roots()) { (void)name; roots.push_back(key); }
  (void)store.closure(roots, true, options.stop, options.progress);
}
Json plan_native_workspace_unregistration(const Store &store, bool include_untracked, bool verify, OperationOptions options) {
  return prepare(store, include_untracked, verify, options).report;
}
Json unregister_native_workspace(Store &store, std::string_view expected_plan, bool include_untracked, OperationOptions options) {
  throw_if_cancelled(options.stop); validate_id(expected_plan);
  auto plan = prepare(store, include_untracked, true, options);
  require(plan.report["plan"].get<std::string>() == expected_plan,
      "native workspace unregistration plan changed; inspect the plan and retry");
  require(plan.report["ready"].get<bool>(), "native workspace unregistration is blocked; inspect the plan");
  auto ingest = store.settings().ingest_options(); ingest.stop = options.stop; ingest.progress = options.progress;
  for (const auto &file : plan.capture.working) {
    auto &entry = plan.capture.files.at(file.relative);
    if (!entry.manifest.empty()) continue;
    const auto imported = ingest_native_file(store, file, ingest, "working file changed during workspace unregistration");
    const auto metadata = store.metadata(Kind::manifest, imported["manifest"].get<std::string>());
    require(metadata["size"].get<std::uint64_t>() == entry.content.size &&
                metadata["file_hash"].get<std::string>() == entry.content.hash,
        "working file changed during workspace unregistration");
    entry.manifest = imported["manifest"].get<std::string>();
  }
  Json files = Json::object();
  for (const auto &[name, entry] : plan.capture.files)
    files[name] = Json{{"manifest", entry.manifest}, {"executable", entry.executable}};
  Json parents = Json::array({plan.archive.head});
  if (plan.archive.index && *plan.archive.index != plan.archive.head) parents.push_back(*plan.archive.index);
  plan.archive.work = store.put(Kind::revision, bytes(Json{{"version", 1}, {"parents", parents},
      {"message", "Syrnck workspace archive work v1: " + plan.archive.name}, {"timestamp", 0}, {"files", files}}.dump())).first.id;
  plan.archive.nonce = hex(random_bytes(32)); plan.archive.created_at = unix_seconds();
  plan.archive.id = store.put(Kind::chunk, bytes(plan.archive.json().dump())).first.id;
  report_progress(options, {ProgressPhase::publishing, "workspace-unregister"});
  const auto final = prepare(store, include_untracked, true, {options.stop});
  require(final.report["plan"] == plan.report["plan"] && final.report["ready"].get<bool>(),
      "native workspace unregistration plan changed; inspect the plan and retry");
  std::vector<RefUpdate> updates;
  for (const auto &[name, key] : plan.archive.references) updates.push_back({name, key, {}});
  for (const auto &[name, key] : plan.archive.roots()) updates.push_back({name, {}, key});
  const auto branch_ref = native_revision_reference(store, plan.archive.branch.value_or(std::string(detached_native_target)));
  if (plan.archive.branch) updates.push_back({branch_ref, ObjectKey{Kind::revision, plan.archive.head}, ObjectKey{Kind::revision, plan.archive.head}});
  require(store.compare_update_refs(updates, {options.stop}), "native workspace references changed; retry operation");
  store.use_native_workspace("");
  auto result = plan.report;
  result["archive"] = plan.archive.id; result["state"] = "unregistered";
  result["working_revision"] = plan.archive.work;
  return result;
}
Json native_workspace_archive(const Store &store, std::string_view id, bool verify, OperationOptions options) {
  const auto archive = read_native_workspace_archive(store, store.refs(), id, options);
  if (verify) verify_native_workspace_archive(store, archive, options);
  auto result = archive.json(); result["id"] = archive.id; result["roots"] = archive.roots().size();
  result["objects_verified"] = verify; return result;
}
Json native_workspace_archives(const Store &store, const std::optional<std::string> &after,
    std::size_t limit, bool verify, OperationOptions options) {
  throw_if_cancelled(options.stop);
  require(limit >= 1 && limit <= 1000, "native workspace archive page limit must be in 1..1000");
  if (after) validate_id(*after);
  const auto refs = store.refs(); Json rows = Json::array(); bool more = false;
  std::size_t budget = 4096;
  const auto prefix = std::string(native_workspace_archive_prefix);
  for (auto at = refs.lower_bound(prefix); at != refs.end() && at->first.starts_with(prefix); ++at) {
    if (!at->first.ends_with("/record")) continue;
    const auto id = at->first.substr(prefix.size(), at->first.size() - prefix.size() - 7);
    if (after && id <= *after) continue;
    const auto archive = read_native_workspace_archive(store, refs, id, options);
    if (rows.size() == limit) { more = true; break; }
    if (verify) verify_native_workspace_archive(store, archive, options);
    auto row = Json{{"id", id}, {"name", archive.name}, {"revision", archive.head}, {"work", archive.work},
        {"index", archive.index ? Json(*archive.index) : Json{}}, {"created_at", archive.created_at}};
    const auto size = row.dump().size() + 1;
    require(size <= max_object_size - budget, "native workspace archive report exceeds 16 MiB");
    budget += size; rows.push_back(std::move(row));
  }
  const auto next = more && !rows.empty() ? rows.back()["id"] : Json{};
  return Json{{"version", 1}, {"archives", rows}, {"has_more", more}, {"next_after", next}, {"objects_verified", verify}};
}
} // namespace syrnck
