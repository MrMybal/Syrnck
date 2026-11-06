#include "internal.hpp"
#include <syrnck/backup.hpp>
#include <syrnck/retention.hpp>
namespace syrnck {
namespace {
struct Prepared {
  Json report;
  std::vector<RefUpdate> updates;
};
Prepared prepare(const Store &store, const Store &backup, const std::vector<std::string> &names,
                 OperationOptions options) {
  throw_if_cancelled(options.stop);
  require(store.root() != backup.root(), "archive retirement requires a separate backup store");
  require(!names.empty() && names.size() <= 1024, "archive retirement requires 1..1024 references");
  require_no_native_checkout(store);
  const auto verified = verify_backup(backup, options);
  const auto refs = store.refs();
  const std::set<std::string> sorted(names.begin(), names.end());
  require(sorted.size() == names.size(), "duplicate archive retirement reference");
  Prepared result;
  Json selected = Json::array();
  std::vector<ObjectKey> roots;
  for (const auto &name : sorted) {
    validate_ref(name);
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
    selected.push_back(Json{{"reference", name}, {"revision", found->second.id}, {"kind", kind}});
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
