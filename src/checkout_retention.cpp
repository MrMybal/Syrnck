#include "native_workspace.hpp"
#include <syrnck/backup.hpp>
#include <syrnck/checkout_retention.hpp>
namespace syrnck {
namespace {
std::optional<ObjectKey> lookup(const Refs &refs, const std::string &name) {
  const auto found = refs.find(name);
  return found == refs.end() ? std::optional<ObjectKey>{} : std::optional{found->second};
}
std::string refs_hash(const Refs &refs) {
  return hash(bytes(encode_refs(refs).dump()));
}
struct Completed {
  std::string id, context;
  ObjectKey target{Kind::revision, {}};
  std::optional<ObjectKey> receipt, source;
  std::string completed_name() const {
    return native_context_reference(context, "checkouts/completed/" + id);
  }
  std::string receipt_name() const {
    return native_context_reference(context, "checkouts/receipts/" + id);
  }
  std::string source_name() const {
    return "archive/checkout/" + id;
  }
  Json json() const {
    return Json{{"id", id},
                {"target", target.id},
                {"source", source ? Json(source->id) : Json{}},
                {"receipt", receipt ? Json(receipt->id) : Json{}},
                {"retained_only", !receipt.has_value()}};
  }
};
Completed read(const Store &store, const Refs &refs, const std::string &id, OperationOptions options) {
  throw_if_cancelled(options.stop);
  validate_id(id);
  Completed value;
  value.id = id;
  value.context = store.native_workspace_name();
  const auto target = lookup(refs, value.completed_name());
  require(target && target->kind == Kind::revision, "unknown completed native checkout target");
  value.target = *target;
  value.receipt = lookup(refs, value.receipt_name());
  value.source = lookup(refs, value.source_name());
  require(!value.source || value.source->kind == Kind::revision, "invalid completed native checkout source");
  if (value.receipt) {
    require(value.receipt->kind == Kind::chunk, "invalid completed native checkout receipt");
    const auto info = read_native_checkout_receipt(store, *value.receipt, id);
    require(info.value("context", std::string{}) == value.context, "invalid completed native checkout receipt");
    require(info["target"] == value.target.id, "native checkout receipt and target disagree");
    if (value.source)
      require(info["source"] == value.source->id, "native checkout receipt and source disagree");
  }
  const auto pending_name = store.native_workspace_reference("checkouts/pending/" + id) + '/';
  const auto pending = refs.lower_bound(pending_name);
  require(pending == refs.end() || !pending->first.starts_with(pending_name),
          "completed native checkout still has pending pins");
  const auto parent = store.root() / "checkouts";
  const auto status = fs::symlink_status(parent);
  if (status.type() != fs::file_type::not_found) {
    require(fs::is_directory(status) && !native_link_like(parent, status),
            "invalid native checkout directory");
    require(fs::symlink_status(parent / id).type() == fs::file_type::not_found,
            "completed native checkout still has private recovery data");
  }
  return value;
}
struct Prepared {
  Json report;
  std::vector<RefUpdate> updates;
};
Prepared prepare(const Store &store, const Store &backup, const std::vector<std::string> &ids, bool sources,
                 OperationOptions options) {
  throw_if_cancelled(options.stop);
  require_no_native_checkout(store);
  require(store.root() != backup.root(), "checkout retirement requires a separate backup store");
  require(!ids.empty() && ids.size() <= 1024, "checkout retirement requires 1..1024 transaction IDs");
  const std::set<std::string> sorted(ids.begin(), ids.end());
  require(sorted.size() == ids.size(), "duplicate checkout retirement transaction ID");
  for (const auto &id : sorted)
    validate_id(id);
  const auto verified = verify_backup(backup, options);
  const auto refs = store.refs();
  Prepared result;
  Json rows = Json::array(), names = Json::array();
  std::set<ObjectKey> roots;
  for (const auto &id : sorted) {
    const auto entry = read(store, refs, id, options);
    rows.push_back(entry.json());
    roots.insert(entry.target);
    result.updates.push_back({entry.completed_name(), entry.target, {}});
    names.push_back(entry.completed_name());
    if (entry.receipt) {
      result.updates.push_back({entry.receipt_name(), entry.receipt, {}});
      names.push_back(entry.receipt_name());
    }
    if (sources && entry.source) {
      roots.insert(*entry.source);
      result.updates.push_back({entry.source_name(), entry.source, {}});
      names.push_back(entry.source_name());
    }
  }
  const auto covered = backup.closure(std::vector<ObjectKey>(roots.begin(), roots.end()), true, options.stop,
                                      options.progress);
  const Json identity{{"version", 1},
                      {"type", "native-checkout-retirement"},
                      {"store", utf8(store.root())},
                      {"references_hash", refs_hash(refs)},
                      {"backup_snapshot", verified["snapshot"]},
                      {"include_source_archives", sources},
                      {"transactions", rows},
                      {"references", names}};
  result.report = identity;
  result.report["plan"] = hash(bytes(identity.dump()));
  result.report["protected_objects"] = covered.size();
  result.report["applied"] = false;
  require(result.report.dump().size() <= max_object_size, "checkout retirement report exceeds 16 MiB");
  return result;
}
} // namespace
Json native_checkout_receipts(const Store &store, const std::optional<std::string> &after, std::size_t limit,
                              bool verify_objects, OperationOptions options) {
  throw_if_cancelled(options.stop);
  require_no_native_checkout(store);
  require(limit >= 1 && limit <= 1000, "native checkout receipt page limit must be in 1..1000");
  if (after)
    validate_id(*after);
  const auto refs = store.refs();
  std::set<std::string> ids;
  for (const auto &prefix : {store.native_workspace_reference("checkouts/completed") + '/',
                              store.native_workspace_reference("checkouts/receipts") + '/'})
    for (auto at = refs.lower_bound(prefix); at != refs.end() && at->first.starts_with(prefix); ++at) {
      throw_if_cancelled(options.stop);
      const auto id = at->first.substr(prefix.size());
      validate_id(id);
      if (!after || id > *after)
        ids.insert(id);
    }
  Json rows = Json::array();
  std::set<ObjectKey> roots;
  for (const auto &id : ids) {
    if (rows.size() == limit)
      break;
    const auto entry = read(store, refs, id, options);
    rows.push_back(entry.json());
    roots.insert(entry.target);
    if (entry.source)
      roots.insert(*entry.source);
    report_progress(options, {ProgressPhase::planning, id, rows.size()});
  }
  if (verify_objects)
    (void)store.closure(std::vector<ObjectKey>(roots.begin(), roots.end()), true, options.stop,
                        options.progress);
  const bool more = ids.size() > rows.size();
  return Json{{"version", 1},
              {"transactions", rows},
              {"has_more", more},
              {"next_after", more ? rows.back()["id"] : Json{}},
              {"objects_verified", verify_objects}};
}
Json plan_checkout_retirement(const Store &store, const Store &backup, const std::vector<std::string> &ids,
                              bool sources, OperationOptions options) {
  return prepare(store, backup, ids, sources, options).report;
}
Json retire_checkouts(const Store &store, const Store &backup, const std::vector<std::string> &ids,
                      std::string_view expected_plan, bool sources, OperationOptions options) {
  throw_if_cancelled(options.stop);
  validate_id(expected_plan);
  // Observers run before final input verification. There are no callbacks
  // between backup/source validation and the atomic reference publication.
  report_progress(options, {ProgressPhase::publishing, "checkout-retirement", 0, ids.size()});
  auto prepared = prepare(store, backup, ids, sources, {options.stop});
  require(prepared.report["plan"].get<std::string>() == expected_plan,
          "checkout retirement plan changed; refresh the plan");
  require(store.compare_update_refs(prepared.updates, {options.stop}),
          "checkout retirement references changed; retry operation");
  prepared.report["applied"] = true;
  return std::move(prepared.report);
}
} // namespace syrnck
