#include "internal.hpp"
#include "native_branch_archive.hpp"
#include "native_tag_archive.hpp"
#include <syrnck/backup.hpp>
#include <syrnck/retention.hpp>
namespace syrnck {
namespace {
struct Prepared {
  Json report;
  std::vector<RefUpdate> updates;
};
struct Group {
  std::string kind, name;
  ObjectKey target;
  Refs roots;
};
Group group(const Store &store, const Refs &refs, std::string_view selector, OperationOptions options) {
  constexpr std::string_view branch_prefix = "archive/branches/", tag_prefix = "archive/tags/";
  const bool branch = selector.starts_with(branch_prefix);
  const auto id = selector.substr(branch ? branch_prefix.size() : tag_prefix.size());
  require(id.size() == 64 && id.find('/') == id.npos,
          "structured archive retirement requires a complete group selector");
  validate_id(id);
  if (branch) {
    const auto archive = read_native_branch_archive(store, refs, id, options);
    (void)archive.state(store, options); // Validate frozen index name/base binding too.
    return {"branch-group", archive.name, archive.target, archive.roots()};
  }
  const auto archive = read_native_tag_archive_roots(store, refs, id, options);
  return {"tag-group", archive.name, archive.target, archive.roots};
}
Prepared prepare(const Store &store, const Store &backup, const std::vector<std::string> &names,
                 OperationOptions options) {
  throw_if_cancelled(options.stop);
  require(store.root() != backup.root(), "archive retirement requires a separate backup store");
  require(!names.empty() && names.size() <= 1024, "archive retirement requires 1..1024 references");
  require_no_native_checkout(store);
  const auto verified = verify_backup(backup, options);
  const auto refs = store.refs();
  const auto backup_refs = backup.refs();
  const std::set<std::string> sorted(names.begin(), names.end());
  require(sorted.size() == names.size(), "duplicate archive retirement reference");
  Prepared result;
  Json selected = Json::array();
  std::vector<ObjectKey> roots;
  std::size_t budget = 4096;
  for (const auto &name : sorted) {
    validate_ref(name);
    if (name.starts_with("archive/branches/") || name.starts_with("archive/tags/")) {
      const auto source = group(store, refs, name, options);
      const auto saved = group(backup, backup_refs, name, options);
      require(source.roots == saved.roots, "backup archive group does not match selected archive");
      auto row = Json{{"reference", name},
                      {"revision", source.target.id},
                      {"kind", source.kind},
                      {"name", source.name},
                      {"roots", encode_refs(source.roots)}};
      const auto size = row.dump().size() + 1;
      require(size <= max_object_size - budget, "archive retirement report exceeds 16 MiB");
      budget += size;
      selected.push_back(std::move(row));
      for (const auto &[member, key] : source.roots) {
        roots.push_back(key);
        result.updates.push_back({member, key, {}});
      }
      continue;
    }
    const auto found = refs.find(name);
    require(found != refs.end(), "unknown archive retirement reference");
    require(found->second.kind == Kind::revision, "archive retirement requires revision roots");
    std::string kind;
    if (name.starts_with("native-fetch/")) {
      const auto id = name.substr(13);
      validate_id(id);
      require(id == found->second.id, "native fetch retirement identity mismatch");
      kind = "fetch-root";
    } else {
      const auto info = store.native_archive(name, false, options);
      kind = info["archive"]["kind"].get<std::string>();
    }
    auto row = Json{{"reference", name}, {"revision", found->second.id}, {"kind", kind}};
    const auto size = row.dump().size() + 1;
    require(size <= max_object_size - budget, "archive retirement report exceeds 16 MiB");
    budget += size;
    selected.push_back(std::move(row));
    roots.push_back(found->second);
    result.updates.push_back({name, found->second, {}});
  }
  const auto protected_objects = backup.closure(roots, true, options.stop, options.progress);
  const auto identity = Json{{"version", 1},
                             {"references_hash", hash(bytes(encode_refs(refs).dump()))},
                             {"backup_snapshot", verified["snapshot"]},
                             {"references", selected}};
  result.report = identity;
  result.report["plan"] = hash(bytes(identity.dump()));
  result.report["protected_objects"] = protected_objects.size();
  result.report["removed_references"] = result.updates.size();
  result.report["applied"] = false;
  return result;
}
} // namespace
Json plan_archive_retirement(const Store &store, const Store &backup, const std::vector<std::string> &names,
                             OperationOptions options) {
  return prepare(store, backup, names, options).report;
}
Json retire_archives(const Store &store, const Store &backup, const std::vector<std::string> &names,
                     std::string_view expected_plan, OperationOptions options) {
  throw_if_cancelled(options.stop);
  validate_id(expected_plan);
  // No observer may invalidate coverage after the final backup verification.
  report_progress(options, {ProgressPhase::publishing, "archive-retirement", 0, names.size()});
  auto prepared = prepare(store, backup, names, {options.stop});
  require(prepared.report["plan"].get<std::string>() == expected_plan,
          "archive retirement plan changed; refresh the plan");
  prepared.report["applied"] = true;
  require(store.compare_update_refs(prepared.updates, {options.stop}),
          "archive retirement references changed; retry operation");
  return std::move(prepared.report);
}
} // namespace syrnck
