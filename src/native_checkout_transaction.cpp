#include "native_workspace.hpp"
#include "native_checkout_layout.hpp"
#include "native_hard_reset.hpp"
#include "native_stash.hpp"
#include "native_context.hpp"
#include "native_materialization_transition.hpp"
#include "native_workspace_relocation.hpp"
#include "io_checkpoint.hpp"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <iterator>
#include <thread>
#include <tuple>
namespace syrnck {
namespace {
constexpr std::string_view active_name = "checkouts/active";
constexpr std::string_view head_name = "workspace/HEAD";
std::optional<ObjectKey> lookup(const Refs &refs, std::string_view name) {
  const auto found = refs.find(std::string(name));
  return found == refs.end() ? std::optional<ObjectKey>{} : std::optional{found->second};
}
bool inside(const fs::path &path, const fs::path &root) {
  auto p = path.begin();
  for (auto r = root.begin(); r != root.end(); ++r, ++p)
    if (p == path.end() || *p != *r)
      return false;
  return true;
}
bool present(const fs::path &path) {
  return fs::symlink_status(path).type() != fs::file_type::not_found;
}
void directory(const fs::path &path, bool create) {
  if (!present(path) && create) {
    require(fs::create_directory(path), "cannot create native checkout directory");
#ifndef _WIN32
    fs::permissions(path, fs::perms::owner_all);
#endif
    sync_directory(path.parent_path());
  }
  const auto status = fs::symlink_status(path);
  require(fs::is_directory(status) && !native_link_like(path, status), "invalid native checkout directory");
}
void remove_empty_directory(const fs::path &path, OperationOptions options) {
  std::error_code error;
  for (unsigned attempt = 0;; ++attempt) {
    throw_if_cancelled(options.stop);
    directory(path, false);
    const auto removed = fs::remove(path, error);
    if (!error) {
      require(removed, "cannot clean native checkout data");
      return;
    }
#ifdef _WIN32
    // Windows may briefly retain delete-pending files after successful removal.
    // Retry only this nonrecursive removal; never delete newly appeared entries.
    if (error == std::errc::directory_not_empty && attempt < 50) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }
#else
    (void)attempt;
#endif
    throw fs::filesystem_error("remove", path, error);
  }
}
struct Head {
  std::string branch = "main";
  fs::path workspace;
  std::optional<ObjectKey> key;
  Json json() const {
    return Json{{"version", native_target_is_detached(branch) ? 2 : 1}, {"type", "native-head"},
                {"directory", utf8(workspace)}, {"branch", native_target_is_detached(branch) ? Json{} : Json(branch)}};
  }
};
Head read_head(const Store &store, const std::optional<ObjectKey> &key) {
  Head head;
  if (!key)
    return head;
  require(key->kind == Kind::chunk, "invalid native HEAD");
  const auto data = store.get(*key);
  const auto value = parse_json(data);
  require(value.is_object() && value.size() == 4 &&
              (value.value("version", 0) == 1 || value.value("version", 0) == 2) &&
              value.value("type", "") == "native-head",
          "invalid native HEAD");
  head.key = key;
  if (value.at("version") == 2) {
    require(value.at("branch").is_null(), "invalid native HEAD");
    head.branch = detached_native_target;
  } else
    head.branch = value.at("branch").get<std::string>();
  head.workspace = path_from_utf8(value.at("directory").get<std::string>());
  validate_ref(native_revision_reference(head.branch));
  validate_ref(native_staging_reference(head.branch));
  require(head.workspace.is_absolute() && head.workspace == head.workspace.lexically_normal() &&
              head.workspace != store.root() &&
              (store.native_workspace_name().empty() ? inside(store.root(), head.workspace)
                  : read_native_workspace_context(store, store.native_workspace_name()).directory == head.workspace) &&
              head.json().dump() == text(data),
          "invalid native HEAD");
  return head;
}
Head read_head(const Store &store) {
  const auto key = lookup(store.refs(), store.native_workspace_reference(head_name));
  require(store.native_workspace_name().empty() || key.has_value(), "invalid native workspace registration");
  return read_head(store, key);
}
void head_workspace(const Head &head, const fs::path &workspace) {
  require(!head.key || head.workspace == fs::canonical(workspace),
          "native HEAD belongs to another workspace");
}
struct Record {
  std::string id, nonce, branch, target, state = "preparing";
  std::string context;
  std::optional<std::string> owner;
  NativeMaterialization source_materialization, target_materialization;
  fs::path workspace;
  std::optional<std::string> source;
  std::optional<std::string> destination_branch;
  std::optional<ObjectKey> previous_head, next_head;
  bool namespace_transition = false, hard_reset = false, restore_worktree = false, pop_stash = false;
  bool materialization_transition = false;
  std::optional<ObjectKey> target_index;
  std::string stash_id, stash_work, stash_record;
  std::optional<ObjectKey> original_head, original_index;
  std::set<std::string> source_directories;
  ObjectKey key{Kind::chunk, {}};
  Json identity() const {
    const auto layout = materialization_transition ? 7 : restore_worktree       ? 6
                                   : hard_reset           ? 5
                                   : namespace_transition ? (destination_branch ? 4 : 3)
                                                          : (destination_branch ? 2 : 1);
    const bool detached = native_target_is_detached(branch) ||
        (destination_branch && native_target_is_detached(*destination_branch));
    const bool partial = !source_materialization.complete() || !target_materialization.complete();
    auto result = Json{{"version", owner ? 11 : materialization_transition ? 10 : partial ? 9 : !context.empty() ? 8 : detached ? 7 : layout},
                       {"type", "native-checkout"},
                       {"nonce", nonce},
                       {"directory", utf8(workspace)},
                       {"branch", branch},
                       {"source", source ? Json(*source) : Json{}},
                       {"target", target}};
    if (owner || partial || detached || !context.empty())
      result["layout_version"] = layout;
    if (owner || partial || !context.empty()) result["context"] = context;
    if (owner || partial) {
      result["source_materialization"] = source_materialization.definition();
      result["target_materialization"] = target_materialization.definition();
    }
    if (destination_branch) {
      result["destination_branch"] = *destination_branch;
      result["previous_head"] = previous_head ? Json(previous_head->id) : Json{};
      result["next_head"] = next_head->id;
    }
    if (hard_reset || restore_worktree || materialization_transition) {
      result["original_head"] = original_head->id;
      result["original_index"] = original_index ? Json(original_index->id) : Json{};
      result["previous_head"] = previous_head ? Json(previous_head->id) : Json{};
      result["source_directories"] = source_directories;
      result["namespace_transition"] = namespace_transition;
    } else if (namespace_transition)
      result["namespace_transition"] = true;
    if (restore_worktree) {
      result["target_index"] = target_index ? Json(target_index->id) : Json{};
      result["stash"] = stash_id;
      result["stash_work"] = stash_work;
      result["stash_record"] = stash_record;
      result["pop"] = pop_stash;
    }
    if (owner) result["owner"] = *owner;
    return result;
  }
  Json json() const {
    auto result = identity();
    result["id"] = id;
    result["state"] = state;
    return result;
  }
  ObjectKey destination() const {
    return {Kind::revision, target};
  }
  std::optional<ObjectKey> origin() const {
    return source ? std::optional{ObjectKey{Kind::revision, *source}} : std::optional<ObjectKey>{};
  }
  std::optional<ObjectKey> branch_origin() const {
    return hard_reset || restore_worktree ? original_head : origin();
  }
  std::optional<ObjectKey> branch_destination() const {
    return restore_worktree ? original_head : std::optional{destination()};
  }
  NativeStashEntry stash() const {
    NativeStashEntry entry;
    entry.id = stash_id;
    entry.work = stash_work;
    entry.record = {Kind::chunk, stash_record};
    return entry;
  }
  std::optional<ObjectKey> index_origin() const {
    return hard_reset || restore_worktree ? original_index : std::optional<ObjectKey>{};
  }
  std::string pin(std::string_view suffix) const {
    return native_context_reference(context, "checkouts/pending/" + id + '/' + std::string(suffix));
  }
  std::string archive() const {
    return "archive/checkout/" + id;
  }
  std::string receipt() const {
    return native_context_reference(context, "checkouts/receipts/" + id);
  }
  std::string retained() const {
    return native_context_reference(context, "checkouts/completed/" + id);
  }
  std::string active_reference() const { return native_context_reference(context, active_name); }
  std::string head_reference() const { return native_context_reference(context, head_name); }
  std::string materialization_reference() const { return native_context_reference(context, "workspace/materialization"); }
  std::optional<ObjectKey> policy_root(bool to_target) const {
    const auto &policy = to_target ? target_materialization : source_materialization;
    return policy.complete() ? std::optional<ObjectKey>{}
        : std::optional{ObjectKey{Kind::chunk, policy.identity()}};
  }
  std::optional<ObjectKey> materialization_root() const {
    return policy_root(materialization_transition && state == "committed");
  }
  RefUpdate materialization_guard(bool publish = false) const {
    const auto root = materialization_root();
    return {materialization_reference(), root, materialization_transition && publish ? policy_root(true) : root};
  }
  void freeze_materialization(const NativeMaterialization &policy) {
    source_materialization = policy;
    target_materialization = source_materialization;
  }
  std::string branch_reference() const {
    const auto name = native_revision_reference(branch);
    return native_target_is_detached(branch) ? native_context_reference(context, name) : name;
  }
  std::string index_reference() const { return native_staging_reference(context, branch); }
  std::string destination_reference() const {
    const auto name = native_revision_reference(*destination_branch);
    return native_target_is_detached(*destination_branch) ? native_context_reference(context, name) : name;
  }
  std::string destination_index_reference() const {
    return native_staging_reference(context, *destination_branch);
  }
  fs::path data(const Store &store) const {
    return store.root() / "checkouts" / id;
  }
  ObjectKey save(const Store &store) const {
    return store.put(Kind::chunk, bytes(json().dump())).first;
  }
};
Record parse_record_impl(const Store &store, const ObjectKey &key, bool for_recovery) {
  require(key.kind == Kind::chunk, "invalid native checkout record");
  const auto data = store.get(key);
  const auto value = parse_json(data);
  const auto stored_version = value.value("version", 0);
  const auto version = stored_version == 7 || stored_version == 8 || stored_version == 9 || stored_version == 10 || stored_version == 11
      ? value.value("layout_version", 0) : stored_version;
  const auto extra = stored_version == 11 ? 5u : stored_version == 9 || stored_version == 10 ? 4u : stored_version == 8 ? 2u : stored_version == 7 ? 1u : 0u;
  require(value.is_object() &&
              ((version == 1 && value.size() == 9 + extra) || (version == 2 && value.size() == 12 + extra) ||
               (version == 3 && value.size() == 10 + extra) || (version == 4 && value.size() == 13 + extra) ||
               (version == 5 && value.size() == 14 + extra) || (version == 6 && value.size() == 19 + extra) ||
               (stored_version == 10 && version == 7 && value.size() == 14 + extra)) &&
              value.value("type", "") == "native-checkout",
          "invalid native checkout record");
  Record record;
  if (stored_version == 8 || stored_version == 9 || stored_version == 10 || stored_version == 11) {
    record.context = value.at("context").get<std::string>();
    if (stored_version == 8 || !record.context.empty()) validate_native_workspace_name(record.context);
  }
  if (stored_version == 9 || stored_version == 10 || stored_version == 11) {
    record.source_materialization = NativeMaterialization::from_definition(value.at("source_materialization"));
    record.target_materialization = NativeMaterialization::from_definition(value.at("target_materialization"));
    require(stored_version == 11 ? (version == 1 || version == 3) &&
                    record.source_materialization.definition() == record.target_materialization.definition()
            : stored_version == 10
                ? version == 7 && record.source_materialization.definition() != record.target_materialization.definition()
                : version >= 1 && version <= 6 && !record.source_materialization.complete() &&
                    record.source_materialization.definition() == record.target_materialization.definition(),
            "invalid native checkout record");
    if (stored_version == 10) {
      const auto features = store.required_features();
      require(std::binary_search(features.begin(), features.end(), "materialization-transitions-v1"),
              "native materialization transition requires a supported reader feature");
      record.materialization_transition = true;
    }
  }
  if (stored_version == 11) {
    record.owner = value.at("owner").get<std::string>();
    validate_id(*record.owner);
    const auto features = store.required_features();
    require(std::binary_search(features.begin(), features.end(), "resident-sync-v1"),
            "owned native checkout requires a supported reader feature");
  }
  record.key = key;
  record.id = value.at("id").get<std::string>();
  record.nonce = value.at("nonce").get<std::string>();
  record.branch = value.at("branch").get<std::string>();
  record.target = value.at("target").get<std::string>();
  record.state = value.at("state").get<std::string>();
  record.hard_reset = version == 5;
  record.restore_worktree = version == 6;
  if (record.restore_worktree) {
    record.stash_id = value.at("stash").get<std::string>();
    record.stash_work = value.at("stash_work").get<std::string>();
    record.stash_record = value.at("stash_record").get<std::string>();
    validate_id(record.stash_id);
    validate_id(record.stash_work);
    validate_id(record.stash_record);
    require(value.at("pop").is_boolean(), "invalid native checkout record");
    record.pop_stash = value.at("pop").get<bool>();
    if (!value.at("target_index").is_null()) {
      const auto id = value.at("target_index").get<std::string>();
      validate_id(id);
      record.target_index = ObjectKey{Kind::revision, id};
    }
  }
  record.namespace_transition = version == 3 || version == 4;
  if (record.hard_reset || record.restore_worktree || record.materialization_transition) {
    require(value.at("namespace_transition").is_boolean(), "invalid native checkout record");
    record.namespace_transition = value.at("namespace_transition").get<bool>();
    const auto head = value.at("original_head").get<std::string>();
    validate_id(head);
    record.original_head = ObjectKey{Kind::revision, head};
    if (!value.at("previous_head").is_null()) {
      const auto selection = value.at("previous_head").get<std::string>();
      validate_id(selection);
      record.previous_head = ObjectKey{Kind::chunk, selection};
    }
    if (!value.at("original_index").is_null()) {
      const auto index = value.at("original_index").get<std::string>();
      validate_id(index);
      record.original_index = ObjectKey{Kind::revision, index};
    }
    require(value.at("source_directories").is_array() && value.at("source_directories").size() <= 100000,
            "invalid native checkout record");
    for (const auto &entry : value.at("source_directories")) {
      const auto name = entry.get<std::string>();
      validate_path(name);
      require(record.source_directories.insert(name).second, "invalid native checkout record");
    }
  }
  if (record.namespace_transition)
    require(value.at("namespace_transition") == true, "invalid native checkout record");
  record.workspace = path_from_utf8(value.at("directory").get<std::string>());
  if (!value.at("source").is_null())
    record.source = value.at("source").get<std::string>();
  if (version == 2 || version == 4) {
    record.destination_branch = value.at("destination_branch").get<std::string>();
    validate_ref(record.destination_reference());
    validate_ref(record.destination_index_reference());
    require(*record.destination_branch != record.branch, "invalid native checkout record");
    if (!value.at("previous_head").is_null()) {
      const auto id = value.at("previous_head").get<std::string>();
      validate_id(id);
      record.previous_head = ObjectKey{Kind::chunk, id};
    }
    const auto id = value.at("next_head").get<std::string>();
    validate_id(id);
    record.next_head = ObjectKey{Kind::chunk, id};
  }
  validate_id(record.id);
  validate_id(record.nonce);
  validate_id(record.target);
  if (record.source)
    validate_id(*record.source);
  validate_ref(record.branch_reference());
  validate_ref(record.index_reference());
  require(
      record.state == "preparing" || record.state == "applying" || record.state == "aborting" ||
          record.state == "discarding" || record.state == "aborted" || record.state == "committed" ||
          (record.namespace_transition && (record.state == "evacuating" || record.state == "installing" ||
                                           record.state == "uninstalling" || record.state == "restoring")),
      "invalid native checkout record");
  require(!record.namespace_transition || (record.state != "applying" && record.state != "aborting"),
          "invalid native checkout record");
  require(record.workspace.is_absolute() && record.workspace == record.workspace.lexically_normal() &&
              record.workspace != store.root() &&
              (record.context.empty() ? inside(store.root(), record.workspace) : !inside(record.workspace, store.root())) &&
              record.id == hash(bytes(record.identity().dump())) && record.json().dump() == text(data),
          "invalid native checkout record");
  if (for_recovery) {
    require(record.context == store.native_workspace_name(), "invalid native checkout record");
    if (!record.context.empty())
      require(read_native_workspace_context(store, record.context).directory == record.workspace,
              "invalid native checkout record");
  }
  if (record.hard_reset || record.restore_worktree || record.materialization_transition)
    require(record.source.has_value(), "invalid native checkout record");
  if (record.materialization_transition) {
    require(record.origin() == record.original_head && record.destination() == record.original_head &&
                !record.original_index && record.source_directories.empty(), "invalid native checkout record");
    if (for_recovery) {
      const auto selection = read_head(store, record.previous_head);
      require(selection.branch == record.branch && (!selection.key || selection.workspace == record.workspace),
              "invalid native checkout record");
    }
  }
  if (record.restore_worktree)
    require(record.origin() == record.original_head && !record.original_index &&
                record.source_directories.empty(),
            "invalid native checkout record");
  if (for_recovery && (record.hard_reset || record.restore_worktree)) {
    const auto selection = read_head(store, record.previous_head);
    require(selection.branch == record.branch && (!selection.key || selection.workspace == record.workspace),
            "invalid native checkout record");
    require(record.source.has_value(), "invalid native checkout record");
    if (record.hard_reset) {
      const auto recovery = store.metadata(Kind::revision, *record.source);
      Json parents = Json::array({record.original_head->id});
      if (record.original_index && record.original_index != record.original_head)
        parents.push_back(record.original_index->id);
      require(recovery["parents"] == parents && recovery["timestamp"] == 0 &&
                  recovery["message"] == "Syrnck hard reset recovery v1: " + record.branch,
              "invalid native checkout record");
    } else {
      const auto target = store.metadata(Kind::revision, record.target);
      const auto index =
          record.target_index ? std::optional{record.target_index->id} : std::optional<std::string>{};
      require(target["parents"] ==
                      native_stash_working_parents(record.original_head->id, record.stash_work, index) &&
                  target["timestamp"] == 0 &&
                  target["message"] == "Syrnck stash working v1: " + record.branch,
              "invalid native checkout record");
      if (record.target_index) {
        const auto staged = store.metadata(Kind::revision, record.target_index->id);
        require(staged["parents"] == Json::array({record.original_head->id}) && staged["timestamp"] == 0 &&
                    staged["message"] == "Syrnck staging v1: " + record.branch,
                "invalid native checkout record");
      }
    }
  }
  return record;
}
Record parse_record(const Store &store, const ObjectKey &key, bool for_recovery = true) {
  try { return parse_record_impl(store, key, for_recovery); }
  catch (const Json::exception &) { throw Error("invalid native checkout record"); }
}
std::optional<Record> active(const Store &store) {
  const auto refs = store.refs();
  const auto key = lookup(refs, store.native_workspace_reference(active_name));
  if (!key)
    return {};
  auto record = parse_record(store, *key);
  require(lookup(refs, record.pin("target")) == record.destination() &&
              lookup(refs, record.pin("source")) == record.origin(),
          "native checkout retention changed");
  if (record.materialization_transition)
    require(lookup(refs, record.pin("source-policy")) == record.policy_root(false) &&
                lookup(refs, record.pin("target-policy")) == record.policy_root(true),
            "native checkout retention changed");
  if (record.restore_worktree) {
    const auto entry = record.stash();
    const bool archived = record.state == "committed" && record.pop_stash;
    const auto saved = read_native_stash(store, entry.id, archived);
    require(saved.record == entry.record && saved.work == entry.work &&
                saved.materialization.definition() == record.source_materialization.definition(),
            "invalid native checkout record");
    require(lookup(refs, archived ? entry.archive_name() : entry.name()) == entry.record &&
                lookup(refs, archived ? entry.archive_work() : entry.work_name()) ==
                    ObjectKey{Kind::revision, entry.work},
            "native stash protection reference changed");
  }
  if (record.destination_branch) {
    const auto head = read_head(store, record.next_head);
    require(head.branch == *record.destination_branch && head.workspace == record.workspace,
            "invalid native checkout record");
  }
  return record;
}
NativeSnapshot source_files(const Store &store, const Record &record, OperationOptions options) {
  if (!record.source) return {};
  // Hard-reset recovery is a physical tree, which can include an unselected
  // ancestor obstructing selected descendants. Its full logical HEAD/index
  // remain parent pins rather than entries in this namespace.
  return record.hard_reset ? read_native_snapshot(store, *record.source, options)
      : read_native_materialized_snapshot(store, *record.source, record.source_materialization, options);
}
NativeSnapshot target_files(const Store &store, const Record &record, OperationOptions options) {
  // Stash restores a physical tree. Ancestor obstructions saved outside the
  // projection must be reinstated, without mixing in hidden logical children.
  return record.restore_worktree ? read_native_snapshot(store, record.target, options)
      : read_native_materialized_snapshot(store, record.target, record.target_materialization, options);
}
Json actions(const Store &store, const Record &record, OperationOptions options) {
  const auto difference = record.source_materialization.complete() && record.target_materialization.complete()
      ? compare_native_snapshots(store, record.source, record.target, options)
      : compare_native_file_maps(source_files(store, record, options),
          target_files(store, record, options), options);
  Json result = Json::array();
  for (const auto &row : difference["changes"]) {
    throw_if_cancelled(options.stop);
    if (row["content_changed"].get<bool>() || row["executable_changed"].get<bool>())
      result.push_back(row);
  }
  return result;
}
std::uint64_t touched_bytes(const Json &entries) {
  std::uint64_t total = 0;
  for (const auto &row : entries) {
    const auto amount =
        (row["target"].is_null() ? row["source"] : row["target"])["size"].get<std::uint64_t>();
    require(amount <= UINT64_MAX - total, "native checkout byte count overflow");
    total += amount;
  }
  return total;
}
// Resolve existing components without following links. Complete namespace
// inventories check aliases before application and before reference publication;
// avoid rescanning a whole directory for every individual file move.
fs::path working_path(const Store &store, const Record &record, const std::string &relative, bool create) {
  validate_path(relative);
  require(fs::canonical(record.workspace) == record.workspace, "native checkout workspace changed");
  auto path = record.workspace;
  const auto components = path_from_utf8(relative);
  for (auto it = components.begin(); it != components.end(); ++it) {
    const bool last = std::next(it) == components.end();
    if (present(path)) {
      const auto status = fs::symlink_status(path);
      require(fs::is_directory(status) && !native_link_like(path, status),
              "native checkout path became obstructed");
    }
    path /= *it;
    require(!inside(path, store.root()), "native checkout path became obstructed");
    if (!last) {
      if (!present(path) && create) {
        require(fs::create_directory(path), "cannot create native checkout directory");
        sync_directory(path.parent_path());
      }
      if (present(path)) {
        const auto status = fs::symlink_status(path);
        require(fs::is_directory(status) && !native_link_like(path, status),
                "native checkout path became obstructed");
      }
    } else if (present(path)) {
      const auto status = fs::symlink_status(path);
      require(fs::is_regular_file(status) && !native_link_like(path, status),
              "native checkout path became obstructed");
    }
  }
  return path;
}
class Transaction {
  const Store &store_;
  Bytes buffer_ = Bytes(1024 * 1024);

public:
  Record record;
  Json entries;
  std::optional<NativeCheckoutLayout> layout;
  Transaction(const Store &store, Record value, OperationOptions options)
      : store_(store), record(std::move(value)), entries(actions(store, record, options)) {
    if (record.namespace_transition) {
      const auto source = source_files(store, record, options);
      layout.emplace(source, target_files(store, record, options), options,
                     record.source_directories);
      require(layout->transition, "invalid native checkout record");
    }
  }
  fs::path file(std::string_view area, std::size_t index) const {
    return record.data(store_) / area / (std::to_string(index) + ".bin");
  }
  void private_directories(bool create) const {
    const auto parent = store_.root() / "checkouts";
    if (!present(parent) && !create)
      return;
    directory(parent, create);
    const auto data = record.data(store_);
    if (!present(data) && !create)
      return;
    directory(data, create);
    for (const auto *name : {"new", "old"}) {
      if (present(data / name) || create)
        directory(data / name, create);
    }
  }
  bool matches(const fs::path &path, const Json &summary, const std::string &relative,
               OperationOptions options, bool check_mode = true) {
    if (!present(path))
      return false;
    const auto status = fs::symlink_status(path);
    require(fs::is_regular_file(status) && !native_link_like(path, status), "invalid native checkout file");
    bool executable = false;
#ifndef _WIN32
    executable =
        (status.permissions() & (fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec)) !=
        fs::perms::none;
    if (check_mode && executable != summary["executable"].get<bool>())
      return false;
#else
    (void)check_mode;
#endif
    const auto content = hash_native_file(
        {path, relative, fs::file_size(path), fs::last_write_time(path), executable}, buffer_, options);
    return content.size == summary["size"].get<std::uint64_t>() &&
           content.hash == summary["file_hash"].get<std::string>();
  }
  void state(std::string value, OperationOptions options) {
    auto next = record;
    next.state = std::move(value);
    const auto key = next.save(store_);
    require(store_.compare_update_refs({{record.active_reference(), record.key, key}}, {options.stop}),
            "native checkout references changed");
    next.key = key;
    record = std::move(next);
  }
  void expected_head() const {
    const auto refs = store_.refs();
    require(lookup(refs, record.materialization_reference()) == record.materialization_root(),
            "native checkout references changed");
    require(lookup(refs, record.branch_reference()) == record.branch_origin() &&
                lookup(refs, record.index_reference()) == record.index_origin(),
            "native checkout branch or staging changed");
    if (record.hard_reset || record.restore_worktree || record.materialization_transition)
      require(lookup(refs, record.head_reference()) == record.previous_head, "native checkout branch or staging changed");
    if (record.destination_branch)
      require(lookup(refs, record.head_reference()) == record.previous_head &&
                  lookup(refs, record.destination_reference()) == record.destination() &&
                  !refs.contains(record.destination_index_reference()),
              "native switch HEAD, destination branch or staging changed");
  }
  void prepare(OperationOptions options);
  void apply(OperationOptions options);
  void apply_namespace(OperationOptions options);
  void undo_namespace(OperationOptions options);
  std::optional<fs::path> exact_path(const std::string &, bool create);
  void remove_directories(const std::set<std::string> &, OperationOptions, bool rollback);
  void undo(OperationOptions options);
  void cleanup(OperationOptions options);
  Json run(bool abort, OperationOptions options);
};
void Transaction::prepare(OperationOptions options) {
  private_directories(true);
  require(fs::is_empty(record.data(store_) / "old"), "invalid native checkout backup state");
  for (std::size_t i = 0; i < entries.size(); ++i) {
    throw_if_cancelled(options.stop);
    const auto &row = entries[i], &target = row["target"];
    if (target.is_null())
      continue;
    const auto path = row["path"].get<std::string>();
    const auto staged = file("new", i);
    if (!present(staged))
      store_.restore_file(target["manifest"].get<std::string>(), staged,
                          {options.stop, [&](const ProgressEvent &event) {
                             auto progress = event;
                             progress.path = path;
                             report_progress(options, progress);
                           }});
    else {
      const auto status = fs::symlink_status(staged);
      require(fs::is_regular_file(status) && !native_link_like(staged, status),
              "invalid native checkout file");
    }
    if (target["executable"].get<bool>())
      make_executable(staged);
    require(matches(staged, target, path, options), "native checkout prepared file changed");
  }
  expected_head();
  const auto plan =
      record.materialization_transition
          ? plan_native_materialization_files(store_, record.workspace, record.branch, record.target,
              record.source_materialization, record.target_materialization, options)
          : record.hard_reset
          ? plan_native_checkout_source(store_, record.workspace, record.branch, *record.source,
                                        record.target, record.source_directories, options)
          : record.restore_worktree && !record.target_materialization.complete()
              ? plan_native_checkout_files(store_, record.workspace, record.branch, target_files(store_, record, options), options, true)
              : store_.plan_checkout(record.workspace, record.branch, record.target, false, options);
  require(plan["source"] == (record.source ? Json(*record.source) : Json{}),
          "native checkout references changed");
  for (const auto &conflict : plan["conflicts"])
    require(conflict["reason"] == "checkout_pending", "native checkout workspace is no longer clean");
  state(record.namespace_transition ? "evacuating" : "applying", options);
  SYRNCK_IO_CHECKPOINT("checkout.prepared", store_.root() / "checkout.state");
}
void Transaction::apply(OperationOptions options) {
  const auto total = touched_bytes(entries);
  std::uint64_t completed = 0;
  if (record.namespace_transition) {
    apply_namespace(options);
    completed = total;
  } else
    for (std::size_t i = 0; i < entries.size(); ++i) {
      const auto &row = entries[i], &source = row["source"], &target = row["target"];
      const auto path = row["path"].get<std::string>();
      report_progress(options, {ProgressPhase::exporting, path, completed, total});
      private_directories(false);
      const auto old = file("old", i), next = file("new", i);
      auto working = working_path(store_, record, path, !target.is_null());
      if (!source.is_null()) {
        if (!present(old)) {
          require(matches(working, source, path, options), "working file changed during native checkout");
          working = working_path(store_, record, path, false);
          move_exclusive(working, old);
        }
        require(matches(old, source, path, options), "native checkout backup changed; abort to restore it");
      } else
        require(!present(old), "invalid native checkout backup state");
      working = working_path(store_, record, path, !target.is_null());
      if (!target.is_null()) {
        if (present(next)) {
          require(matches(next, target, path, options), "native checkout prepared file changed");
          working = working_path(store_, record, path, true);
          require(!present(working), "native checkout destination appeared; preserve it before recovery");
          move_exclusive(next, working);
        } else
          require(matches(working, target, path, options), "working file changed during native checkout");
      } else
        require(!present(working), "native checkout destination appeared; preserve it before recovery");
      completed += (target.is_null() ? source : target)["size"].get<std::uint64_t>();
      report_progress(options, {ProgressPhase::exporting, path, completed, total, i + 1 == entries.size()});
    }
  SYRNCK_IO_CHECKPOINT("checkout.applied", store_.root() / "checkout.state");
  report_progress(options, {ProgressPhase::publishing, "checkout", completed, total});
  // This is the last observer. Recheck every desired file after it, then use
  // only stop tokens through the atomic branch/journal publication.
  const OperationOptions final_options{options.stop};
  std::vector<std::string> target_paths, removed_paths;
  for (const auto &[path, target] : target_files(store_, record, final_options)) {
    require(matches(working_path(store_, record, path, false), target.json(), path, final_options),
            "working file changed during native checkout");
    target_paths.push_back(path);
  }
  for (const auto &row : entries)
    if (row["target"].is_null()) {
      if (!record.namespace_transition)
        require(!present(working_path(store_, record, row["path"].get<std::string>(), false)),
                "native checkout destination appeared; preserve it before recovery");
      removed_paths.push_back(row["path"].get<std::string>());
    }
  verify_native_checkout_namespace(store_, record.workspace, target_paths, removed_paths, final_options, true,
                                   record.namespace_transition);
  expected_head();
  auto committed = record;
  committed.state = "committed";
  const auto key = committed.save(store_);
  std::vector<RefUpdate> updates{
      {record.branch_reference(), record.branch_origin(), record.branch_destination()},
      {record.active_reference(), record.key, key},
      {record.receipt(), {}, key},
      {record.retained(), {}, record.destination()}, record.materialization_guard(true)};
  if (record.destination_branch) {
    updates[0].value = record.origin();
    updates.push_back({record.destination_reference(), record.destination(), record.destination()});
    updates.push_back({record.head_reference(), record.previous_head, record.next_head});
    updates.push_back({record.destination_index_reference(), {}, {}});
  }
  if (record.hard_reset || record.restore_worktree || record.materialization_transition) {
    updates.push_back({record.index_reference(), record.original_index, record.target_index});
    updates.push_back({record.head_reference(), record.previous_head, record.previous_head});
  }
  if (record.restore_worktree) {
    const auto extra = native_stash_guard(store_, record.stash(), record.pop_stash);
    updates.insert(updates.end(), extra.begin(), extra.end());
  }
  if (record.source)
    updates.push_back({record.archive(), {}, record.origin()});
  require(store_.compare_update_refs(updates, final_options), "native checkout references changed");
  committed.key = key;
  record = std::move(committed);
  SYRNCK_IO_CHECKPOINT("checkout.committed", store_.root() / "checkout.state");
}
std::optional<fs::path> Transaction::exact_path(const std::string &relative, bool create) {
  validate_path(relative);
  auto parent = record.workspace;
  const auto components = path_from_utf8(relative);
  for (auto i = components.begin(); i != components.end(); ++i) {
    const bool last = std::next(i) == components.end();
    const auto status = fs::symlink_status(parent);
    require(fs::is_directory(status) && !native_link_like(parent, status),
            "native checkout path became obstructed");
    std::optional<fs::path> exact;
    const auto wanted = utf8(*i), folded = portable_fold(wanted);
    bool alias = false;
    for (const auto &entry : fs::directory_iterator(parent)) {
      const auto name = utf8(entry.path().filename());
      if (name == wanted)
        exact = entry.path();
      else if (portable_fold(name) == folded)
        alias = true;
    }
    if (!exact) {
      if (!create)
        return {};
      require(!alias, "native checkout path became obstructed");
      parent /= *i;
      require(!inside(parent, store_.root()), "native checkout path became obstructed");
      if (!last) {
        require(fs::create_directory(parent), "cannot create native checkout directory");
        sync_directory(parent.parent_path());
        SYRNCK_IO_CHECKPOINT("checkout.directory_created", parent);
      }
    } else {
      require(!alias && !inside(*exact, store_.root()), "native checkout path became obstructed");
      const auto entry_status = fs::symlink_status(*exact);
      require(!native_link_like(*exact, entry_status), "native checkout path became obstructed");
      if (!last && !fs::is_directory(entry_status)) {
        require(!create && fs::is_regular_file(entry_status), "native checkout path became obstructed");
        return {};
      }
      parent = *exact;
    }
  }
  return parent;
}
void Transaction::remove_directories(const std::set<std::string> &directories, OperationOptions options,
                                     bool rollback) {
  for (auto i = directories.rbegin(); i != directories.rend(); ++i) {
    throw_if_cancelled(options.stop);
    const auto path = exact_path(*i, false);
    if (!path)
      continue;
    const auto status = fs::symlink_status(*path);
    if (rollback && fs::is_regular_file(status)) {
      bool original = false;
      for (const auto &row : entries)
        if (row["path"].get<std::string>() == *i && !row["source"].is_null())
          original = true;
      require(original, "native checkout path became obstructed");
      continue;
    }
    require(fs::is_directory(status) && !native_link_like(*path, status) && fs::is_empty(*path),
            "native checkout path became obstructed");
    remove_empty_directory(*path, options);
    sync_directory(path->parent_path());
    SYRNCK_IO_CHECKPOINT("checkout.directory_removed", *path);
  }
}
void Transaction::apply_namespace(OperationOptions options) {
  const auto total = touched_bytes(entries);
  if (record.state == "evacuating") {
    for (std::size_t i = 0; i < entries.size(); ++i) {
      const auto &row = entries[i], &source = row["source"];
      if (source.is_null())
        continue;
      const auto path = row["path"].get<std::string>();
      report_progress(options, {ProgressPhase::exporting, path, 0, total});
      private_directories(false);
      const auto old = file("old", i);
      if (!present(old)) {
        const auto working = exact_path(path, false);
        require(working && matches(*working, source, path, options),
                "working file changed during native checkout");
        move_exclusive(*working, old);
      }
      require(matches(old, source, path, options), "native checkout backup changed; abort to restore it");
    }
    remove_directories(layout->remove_directories, options, false);
    state("installing", options);
    SYRNCK_IO_CHECKPOINT("checkout.evacuated", store_.root() / "checkout.state");
  }
  std::uint64_t completed = 0;
  for (std::size_t i = 0; i < entries.size(); ++i) {
    const auto &row = entries[i], &target = row["target"];
    const auto path = row["path"].get<std::string>();
    report_progress(options, {ProgressPhase::exporting, path, completed, total});
    private_directories(false);
    if (!target.is_null()) {
      const auto next = file("new", i);
      if (present(next)) {
        require(matches(next, target, path, options), "native checkout prepared file changed");
        const auto working = *exact_path(path, true);
        require(!present(working), "native checkout destination appeared; preserve it before recovery");
        move_exclusive(next, working);
      } else {
        const auto working = exact_path(path, false);
        require(working && matches(*working, target, path, options),
                "working file changed during native checkout");
      }
    }
    completed += (target.is_null() ? row["source"] : target)["size"].get<std::uint64_t>();
    report_progress(options, {ProgressPhase::exporting, path, completed, total, i + 1 == entries.size()});
  }
}
void Transaction::undo_namespace(OperationOptions options) {
  const auto total = touched_bytes(entries);
  if (record.state == "uninstalling") {
    for (std::size_t n = entries.size(); n != 0; --n) {
      const auto i = n - 1;
      const auto &row = entries[i], &target = row["target"];
      if (target.is_null() || present(file("new", i)))
        continue;
      const auto path = row["path"].get<std::string>();
      report_progress(options, {ProgressPhase::exporting, path, 0, total});
      const auto working = exact_path(path, false);
      require(working && matches(*working, target, path, options),
              "working file changed; preserve it before aborting checkout");
      move_exclusive(*working, file("new", i));
    }
    remove_directories(layout->rollback_directories, options, true);
    state("restoring", options);
    SYRNCK_IO_CHECKPOINT("checkout.uninstalled", store_.root() / "checkout.state");
  }
  for (std::size_t i = 0; i < entries.size(); ++i) {
    const auto &row = entries[i];
    if (row["source"].is_null())
      continue;
    const auto path = row["path"].get<std::string>();
    report_progress(options, {ProgressPhase::exporting, path, 0, total});
    const auto old = file("old", i);
    if (present(old)) {
      const auto status = fs::symlink_status(old);
      require(fs::is_regular_file(status) && !native_link_like(old, status), "invalid native checkout file");
      const auto working = *exact_path(path, true);
      require(!present(working), "native checkout destination appeared; preserve it before recovery");
      move_exclusive(old, working);
    } else {
      const auto working = exact_path(path, false);
      require(working && fs::is_regular_file(fs::symlink_status(*working)),
              "native checkout path became obstructed");
    }
  }
  if (record.hard_reset)
    for (const auto &relative : record.source_directories) {
      throw_if_cancelled(options.stop);
      const auto path = exact_path(relative, true);
      require(path.has_value(), "native checkout path became obstructed");
      if (!present(*path)) {
        require(fs::create_directory(*path), "cannot create native checkout directory");
        sync_directory(path->parent_path());
        SYRNCK_IO_CHECKPOINT("checkout.directory_created", *path);
      }
      const auto status = fs::symlink_status(*path);
      require(fs::is_directory(status) && !native_link_like(*path, status),
              "native checkout path became obstructed");
    }
}
void Transaction::undo(OperationOptions options) {
  if (record.state == "discarding")
    return;
  const auto total = touched_bytes(entries);
  std::uint64_t completed = 0;
  for (std::size_t n = entries.size(); n != 0; --n) {
    const auto i = n - 1;
    const auto &row = entries[i], &source = row["source"], &target = row["target"];
    const auto path = row["path"].get<std::string>();
    report_progress(options, {ProgressPhase::exporting, path, completed, total});
    private_directories(false);
    const auto old = file("old", i), next = file("new", i);
    auto working = working_path(store_, record, path, false);
    if (present(old)) {
      require(!source.is_null(), "invalid native checkout backup state");
      const auto status = fs::symlink_status(old);
      require(fs::is_regular_file(status) && !native_link_like(old, status), "invalid native checkout file");
      if (!target.is_null() && !present(next) && present(working)) {
        require(matches(working, target, path, options),
                "working file changed; preserve it before aborting checkout");
        working = working_path(store_, record, path, false);
        move_exclusive(working, next);
        require(matches(next, target, path, options), "native checkout prepared file changed");
      }
      working = working_path(store_, record, path, true);
      require(!present(working), "native checkout destination appeared; preserve it before recovery");
      // A source edited just before its backup was taken is restored verbatim.
      move_exclusive(old, working);
    } else if (source.is_null()) {
      if (!present(next) && present(working)) {
        require(matches(working, target, path, options),
                "working file changed; preserve it before aborting checkout");
        working = working_path(store_, record, path, false);
        move_exclusive(working, next);
        require(matches(next, target, path, options), "native checkout prepared file changed");
      }
    } else if (!target.is_null())
      require(present(next), "invalid native checkout backup state");
    completed += (target.is_null() ? source : target)["size"].get<std::uint64_t>();
    report_progress(options, {ProgressPhase::exporting, path, completed, total, n == 1});
  }
}
void Transaction::cleanup(OperationOptions options) {
  private_directories(false);
  const auto data = record.data(store_);
  std::vector<fs::path> owned;
  if (present(data)) {
    for (const auto &entry : fs::directory_iterator(data))
      require(entry.path().filename() == "new" || entry.path().filename() == "old",
              "unexpected native checkout data");
    for (const auto *area : {"new", "old"}) {
      const auto parent = data / area;
      if (!present(parent))
        continue;
      for (const auto &entry : fs::directory_iterator(parent)) {
        throw_if_cancelled(options.stop);
        const auto name = utf8(entry.path().filename());
        const auto status = entry.symlink_status();
        require(fs::is_regular_file(status) && !native_link_like(entry.path(), status),
                "unexpected native checkout data");
        const bool temporary =
            std::string_view(area) == "new" && name.size() == 36 && name.starts_with(".tmp") &&
            std::all_of(name.begin() + 4, name.end(),
                        [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
        if (!temporary) {
          std::size_t index = 0;
          const auto end = name.size() >= 4 ? name.data() + name.size() - 4 : name.data();
          const auto parsed = std::from_chars(name.data(), end, index);
          require(parsed.ec == std::errc{} && parsed.ptr == end && index < entries.size() &&
                      std::to_string(index) + ".bin" == name,
                  "unexpected native checkout data");
          const auto &row = entries[index];
          const auto &summary = row[std::string_view(area) == "new" ? "target" : "source"];
          require(!summary.is_null() && (std::string_view(area) == "new" || record.state == "committed"),
                  "invalid native checkout backup state");
          require(matches(entry.path(), summary, row["path"].get<std::string>(), options,
                          record.state != "discarding"),
                  "native checkout private file changed; preserve it before cleanup");
        }
        owned.push_back(entry.path());
      }
    }
    for (const auto &path : owned) {
      throw_if_cancelled(options.stop);
      require(fs::remove(path), "cannot clean native checkout data");
    }
    for (const auto *area : {"new", "old"})
      if (present(data / area))
        remove_empty_directory(data / area, options);
    remove_empty_directory(data, options);
    sync_directory(data.parent_path());
  }
  SYRNCK_IO_CHECKPOINT("checkout.cleaned", store_.root() / "checkout.state");
  const auto refs = store_.refs();
  if (record.state == "committed") {
    require(lookup(refs, record.receipt()) == record.key &&
                lookup(refs, record.retained()) == record.destination() &&
                (!record.source || lookup(refs, record.archive()) == record.origin()),
            "native checkout retention changed");
    if (record.destination_branch)
      require(lookup(refs, record.head_reference()) == record.next_head,
              "native switch HEAD, destination branch or staging changed");
  } else
    expected_head();
  std::vector<RefUpdate> updates{{record.active_reference(), record.key, {}},
      {record.pin("target"), record.destination(), {}}, {record.pin("source"), record.origin(), {}}};
  if (record.materialization_transition) {
    require(lookup(refs, record.materialization_reference()) == record.materialization_root(),
            "native checkout references changed");
    updates.push_back({record.pin("source-policy"), record.policy_root(false), {}});
    updates.push_back({record.pin("target-policy"), record.policy_root(true), {}});
    updates.push_back(record.materialization_guard());
  }
  require(store_.compare_update_refs(updates, {options.stop}),
          "native checkout references changed");
}
Json Transaction::run(bool abort, OperationOptions options) {
  throw_if_cancelled(options.stop);
  require(fs::canonical(record.workspace) == record.workspace, "native checkout workspace changed");
  if (abort) {
    require(record.state != "committed", "native checkout is committed; recovery can only finish cleanup");
    if (record.state == "preparing" || record.state == "applying" || record.state == "evacuating" ||
        record.state == "installing")
      state(record.state == "preparing"   ? "discarding"
            : record.namespace_transition ? "uninstalling"
                                          : "aborting",
            options);
  }
  if (record.state != "committed")
    expected_head();
  if (!record.namespace_transition && (record.state == "applying" || record.state == "aborting")) {
    std::vector<std::string> paths;
    for (const auto &entry : entries)
      paths.push_back(entry["path"].get<std::string>());
    verify_native_checkout_namespace(store_, record.workspace, paths, {}, options, false);
  }
  if (record.state == "preparing")
    prepare(options);
  if (record.state == "applying" || record.state == "evacuating" || record.state == "installing")
    apply(options);
  const bool committed = record.state == "committed";
  if (record.state == "aborting") {
    undo(options);
    state("aborted", options);
  }
  if (record.state == "uninstalling" || record.state == "restoring") {
    undo_namespace(options);
    state("aborted", options);
  }
  // Once branch publication succeeds, cleanup has no observers/cancellation.
  cleanup(committed ? OperationOptions{} : options);
  auto result = record.json();
  result["pending"] = false;
  result["state"] = committed ? "committed" : "aborted";
  result["archive"] = committed && record.source ? Json(record.archive()) : Json{};
  return result;
}
} // namespace
bool validate_native_materialization_transition(const Store &store, std::string_view context,
    const Refs &before, const Refs &after) {
  if (context != store.native_workspace_name()) return false;
  const auto active_ref = native_context_reference(context, active_name);
  const auto old = lookup(before, active_ref), next = lookup(after, active_ref);
  if (!old || !next || old == next) return false;
  const auto source = parse_record(store, *old, false), target = parse_record(store, *next, false);
  if (!source.materialization_transition || !target.materialization_transition ||
      source.context != context || target.identity().dump() != source.identity().dump() ||
      source.state != (source.namespace_transition ? "installing" : "applying") || target.state != "committed")
    return false;
  for (const auto *refs : {&before, &after}) {
    if (lookup(*refs, source.branch_reference()) != source.origin() ||
        lookup(*refs, source.head_reference()) != source.previous_head ||
        lookup(*refs, source.index_reference()) ||
        lookup(*refs, source.pin("source")) != source.origin() ||
        lookup(*refs, source.pin("target")) != source.destination() ||
        lookup(*refs, source.pin("source-policy")) != source.policy_root(false) ||
        lookup(*refs, source.pin("target-policy")) != source.policy_root(true)) return false;
  }
  return lookup(before, source.materialization_reference()) == source.policy_root(false) &&
      lookup(after, source.materialization_reference()) == source.policy_root(true) &&
      !lookup(before, source.receipt()) && !lookup(before, source.retained()) &&
      lookup(after, source.receipt()) == next && lookup(after, source.retained()) == source.destination() &&
      lookup(after, source.archive()) == source.origin();
}
Json read_native_checkout_receipt(const Store &store, const ObjectKey &key, std::string_view id) {
  const auto record = parse_record(store, key, false);
  require(record.id == id && record.state == "committed", "invalid native checkout record");
  return record.json();
}
Json Store::native_head() const {
  const auto head = read_head(*this);
  const auto revision = lookup(refs(), native_revision_reference(*this, head.branch));
  require(!revision || revision->kind == Kind::revision, "branch must point to a revision");
  auto result = Json{{"version", 1},
              {"branch", native_target_is_detached(head.branch) ? Json{} : Json(head.branch)},
              {"revision", revision ? Json(revision->id) : Json{}},
              {"explicit", head.key.has_value()},
              {"directory", head.key ? Json(utf8(head.workspace)) : Json{}}};
  if (native_target_is_detached(head.branch)) {
    require(revision.has_value(), "detached native HEAD is missing its retained revision");
    result["mode"] = "detached";
    result["reference"] = native_revision_reference(*this, head.branch);
  }
  if (!native_workspace_name().empty()) result["workspace"] = native_workspace_name();
  return result;
}
std::string Store::native_target() const {
  const auto head = read_head(*this);
  if (native_target_is_detached(head.branch)) {
    const auto revision = lookup(refs(), native_revision_reference(*this, head.branch));
    require(revision && revision->kind == Kind::revision,
            "detached native HEAD is missing its retained revision");
  }
  return head.branch;
}
Json Store::plan_switch(const fs::path &workspace, std::string_view branch, bool verify_objects,
                        OperationOptions options) const {
  throw_if_cancelled(options.stop);
  validate_ref(native_revision_reference(*this, std::string(branch)));
  validate_ref(native_staging_reference(*this, std::string(branch)));
  const auto head = read_head(*this);
  head_workspace(head, workspace);
  const auto destination = lookup(refs(), native_revision_reference(*this, std::string(branch)));
  require(destination && destination->kind == Kind::revision, "unknown native branch");
  auto plan = plan_checkout(workspace, head.branch, destination->id, verify_objects, options);
  const auto staging = staged_status(branch, options);
  plan["operation"] = "switch";
  plan["destination_branch"] = branch;
  plan["head_object"] = head.key ? Json(head.key->id) : Json{};
  plan["destination_staging_snapshot"] = staging["snapshot"];
  const auto owner = native_branch_workspace_owner(*this, branch);
  if (owner && *owner != native_workspace_name()) {
    plan["conflicts"].push_back(Json{{"path", ""}, {"reason", "branch_in_other_workspace"}, {"obstruction", *owner}});
    plan["ready"] = false;
  }
  if (branch != head.branch && !staging["snapshot"].is_null()) {
    plan["conflicts"].push_back(
        Json{{"path", ""}, {"reason", "target_staging_present"}, {"obstruction", ""}});
    plan["ready"] = false;
    std::sort(plan["conflicts"].begin(), plan["conflicts"].end(), [](const Json &a, const Json &b) {
      return std::tuple{a["path"].get<std::string>(), a["reason"].get<std::string>(),
                        a["obstruction"].get<std::string>()} <
             std::tuple{b["path"].get<std::string>(), b["reason"].get<std::string>(),
                        b["obstruction"].get<std::string>()};
    });
  }
  return plan;
}
Json Store::switch_directory(const fs::path &workspace, std::string_view branch,
                             OperationOptions options) const {
  throw_if_cancelled(options.stop);
  require_no_native_checkout(*this);
  const auto materialization = read_native_materialization(*this);
  const auto head = read_head(*this);
  const auto plan = plan_switch(workspace, branch, false, options);
  require(plan["ready"].get<bool>(), "native switch is blocked; inspect native switch-plan");
  require(plan["branch"] == head.branch && plan["head_object"] == (head.key ? Json(head.key->id) : Json{}),
          "native switch HEAD, destination branch or staging changed");
  if (branch == head.branch)
    return Json{{"version", 1},
                {"pending", false},
                {"state", "unchanged"},
                {"branch", branch},
                {"target", plan["target"]}};
  Record record;
  record.context = native_workspace_name();
  record.freeze_materialization(materialization.policy);
  record.workspace = fs::canonical(workspace);
  record.namespace_transition = plan.value("namespace_transition", false);
  record.nonce = hex(random_bytes(32));
  record.branch = head.branch;
  record.destination_branch = branch;
  record.target = plan["target"].get<std::string>();
  if (!plan["source"].is_null())
    record.source = plan["source"].get<std::string>();
  record.previous_head = head.key;
  Head next{std::string(branch), record.workspace, {}};
  record.next_head = put(Kind::chunk, bytes(next.json().dump())).first;
  record.id = hash(bytes(record.identity().dump()));
  if (present(root() / "checkouts"))
    directory(root() / "checkouts", false);
  require(!present(record.data(*this)), "native checkout transaction directory already exists");
  record.key = record.save(*this);
  std::vector<RefUpdate> updates{{native_workspace_reference(active_name), {}, record.key},
                                 {native_workspace_reference(head_name), head.key, head.key},
                                 {record.branch_reference(), record.origin(), record.origin()},
                                 {native_revision_reference(*this, std::string(branch)), record.destination(), record.destination()},
                                 {record.index_reference(), {}, {}},
                                 {native_staging_reference(*this, std::string(branch)), {}, {}},
                                 {record.pin("target"), {}, record.destination()}, record.materialization_guard()};
  if (record.source)
    updates.push_back({record.pin("source"), {}, record.origin()});
  require(compare_update_refs(updates, options), "native switch HEAD, destination branch or staging changed");
  return Transaction(*this, std::move(record), options).run(false, options);
}
Json Store::plan_detached_checkout(const fs::path &workspace, std::string_view revision, bool verify_objects,
                                   OperationOptions options) const {
  throw_if_cancelled(options.stop);
  validate_id(revision);
  const auto head = read_head(*this);
  head_workspace(head, workspace);
  auto plan = plan_checkout(workspace, head.branch, revision, verify_objects, options);
  plan["operation"] = "switch-detached";
  plan["destination_branch"] = Json{};
  plan["head_object"] = head.key ? Json(head.key->id) : Json{};
  const auto detached = staged_status(detached_native_target, options);
  plan["destination_staging_snapshot"] = detached["snapshot"];
  if (!native_target_is_detached(head.branch) && !detached["snapshot"].is_null()) {
    plan["conflicts"].push_back(Json{{"path", ""}, {"reason", "target_staging_present"}, {"obstruction", ""}});
    plan["ready"] = false;
  }
  return plan;
}
Json Store::detach_checkout(const fs::path &workspace, std::string_view revision, OperationOptions options) const {
  throw_if_cancelled(options.stop);
  require_no_native_checkout(*this);
  const auto materialization = read_native_materialization(*this);
  const auto head = read_head(*this);
  const auto plan = plan_detached_checkout(workspace, revision, false, options);
  require(plan["ready"].get<bool>(), "native detached checkout is blocked; inspect native switch-plan --detach");
  require(lookup(refs(), materialization.reference) == materialization.root,
          "native detached references changed; retry operation");
  if (native_target_is_detached(head.branch)) {
    auto result = checkout_directory(workspace, detached_native_target, revision, options);
    result["operation"] = "switch-detached";
    return result;
  }
  const auto refs = this->refs();
  const auto slot = native_revision_reference(*this, detached_native_target);
  const auto previous = lookup(refs, slot);
  const ObjectKey target{Kind::revision, std::string(revision)};
  std::vector<RefUpdate> updates{{slot, previous, target},
      {native_staging_reference(*this, detached_native_target), {}, {}},
      {native_workspace_reference(head_name), head.key, head.key},
      {materialization.reference, materialization.root, materialization.root}};
  const auto source = plan["source"].is_null() ? std::optional<ObjectKey>{}
      : std::optional{ObjectKey{Kind::revision, plan["source"].get<std::string>()}};
  updates.push_back({native_revision_reference(*this, head.branch), source, source});
  if (previous && *previous != target) {
    const auto archive = "archive/" + previous->id;
    const auto old = lookup(refs, archive);
    require(!old || old == previous, "native detached archive changed");
    updates.push_back({archive, old, previous});
  }
  // Retain/verify the destination before changing visible HEAD or work files.
  // An interrupted preparation leaves the original selection intact, plus a
  // retained candidate; the old detached line is archived before replacement.
  require(compare_update_refs(updates, options), "native detached references changed; retry operation");
  auto result = switch_directory(workspace, detached_native_target, options);
  result["operation"] = "switch-detached";
  return result;
}
void require_no_native_checkout(const Store &store) {
  const auto refs = store.refs();
  require(!refs.contains(std::string(native_relocation_active)),
      "native workspace relocation is pending; run native workspace-relocation-recover");
  require(std::none_of(refs.begin(), refs.end(), [](const auto &entry) {
            return native_checkout_active_reference(entry.first);
          }), "native checkout is pending; run native checkout-recover");
}
static Json checkout_directory_impl(const Store &store, const fs::path &workspace, std::string_view branch,
                                    std::string_view target, std::optional<std::string> owner, OperationOptions options) {
  throw_if_cancelled(options.stop);
  require_no_native_checkout(store);
  const auto materialization = read_native_materialization(store);
  const auto plan = store.plan_checkout(workspace, branch, target, false, options);
  require(plan["ready"].get<bool>(), "native checkout is blocked; inspect native checkout-plan");
  if (plan["source"] == Json(target))
    return Json{{"version", 1}, {"pending", false}, {"state", "unchanged"}, {"target", target}};
  if (owner) store.require_features({"resident-sync-v1"}, options);
  Record record;
  record.context = store.native_workspace_name();
  record.owner = std::move(owner);
  record.freeze_materialization(materialization.policy);
  record.workspace = fs::canonical(workspace);
  record.namespace_transition = plan.value("namespace_transition", false);
  record.nonce = hex(random_bytes(32));
  record.branch = branch;
  record.target = target;
  if (!plan["source"].is_null())
    record.source = plan["source"].get<std::string>();
  record.id = hash(bytes(record.identity().dump()));
  if (present(store.root() / "checkouts"))
    directory(store.root() / "checkouts", false);
  require(!present(record.data(store)), "native checkout transaction directory already exists");
  record.key = record.save(store);
  std::vector<RefUpdate> updates{{store.native_workspace_reference(active_name), {}, record.key},
                                 {record.branch_reference(), record.origin(), record.origin()},
                                 {record.index_reference(), {}, {}},
                                 {record.pin("target"), {}, record.destination()}, record.materialization_guard()};
  if (record.source)
    updates.push_back({record.pin("source"), {}, record.origin()});
  require(store.compare_update_refs(updates, options), "native checkout references changed");
  return Transaction(store, std::move(record), options).run(false, options);
}
Json Store::checkout_directory(const fs::path &workspace, std::string_view branch, std::string_view target,
                               OperationOptions options) const {
  return checkout_directory_impl(*this, workspace, branch, target, {}, options);
}
Json Store::checkout_directory_owned(const fs::path &workspace, std::string_view branch, std::string_view target,
                                     std::string_view owner, OperationOptions options) const {
  validate_id(owner);
  return checkout_directory_impl(*this, workspace, branch, target, std::string(owner), options);
}
Json Store::recover_checkout(bool abort, OperationOptions options) const {
  throw_if_cancelled(options.stop);
  auto record = active(*this);
  if (!record)
    return Json{{"version", 1}, {"pending", false}};
  return Transaction(*this, std::move(*record), options).run(abort, options);
}
Json Store::recover_checkout_owned(const fs::path &workspace, std::string_view branch, std::string_view owner,
                                   OperationOptions options) const {
  throw_if_cancelled(options.stop);
  validate_id(owner);
  validate_ref(native_revision_reference(*this, std::string(branch)));
  validate_native_workspace(workspace);
  auto record = active(*this);
  if (!record) return Json{{"version", 1}, {"pending", false}};
  require(record->owner == owner && record->branch == branch && record->workspace == fs::canonical(workspace),
          "native checkout ownership or workspace does not match");
  return Transaction(*this, std::move(*record), options).run(false, options);
}
Json Store::checkout_status(const std::optional<std::string> &transaction) const {
  auto record = active(*this);
  bool pending = record.has_value();
  if (transaction) {
    validate_id(*transaction);
    if (!record || record->id != *transaction) {
      const auto key = lookup(refs(), native_workspace_reference("checkouts/receipts/" + *transaction));
      require(key.has_value(), "unknown native checkout transaction");
      // Receipts retain the immutable record, source/target IDs and result.
      // Old HEAD selections and explicitly retired source archives are not
      // receipt dependencies. Only active recovery requires those objects.
      record = parse_record(*this, *key, false);
      require(record->id == *transaction && record->state == "committed", "invalid native checkout record");
      pending = false;
    }
  }
  if (!record)
    return Json{{"version", 1}, {"pending", false}};
  auto result = record->json();
  result["pending"] = pending;
  result["data_directory"] = utf8(record->data(*this));
  // An explicitly retired source can be collected while its local receipt
  // remains. Keep immutable status readable without inventing a file diff.
  if (!pending && record->source && !lookup(refs(), record->archive()) && !contains(*record->origin())) {
    result["files"] = Json{};
    result["files_available"] = false;
    return result;
  }
  Json files = Json::array();
  const auto changes = actions(*this, *record, {});
  for (std::size_t i = 0; i < changes.size(); ++i)
    files.push_back(Json{{"path", changes[i]["path"]}, {"slot", std::to_string(i) + ".bin"}});
  result["files"] = std::move(files);
  return result;
}
Json capture_native_hard_reset(const Store &store, const fs::path &workspace, std::string_view branch,
                               std::string_view expected_head, std::string_view target,
                               std::string_view expected_plan, const std::set<std::string> &additional_files,
                               const NativeCapturePublisher &publisher, OperationOptions options) {
  throw_if_cancelled(options.stop);
  validate_id(expected_head);
  validate_id(expected_plan);
  require_no_native_checkout(store);
  auto plan = prepare_native_hard_reset(store, workspace, branch, target, true, options, additional_files);
  require(plan.head == expected_head, "native hard reset tip changed; inspect the plan and retry");
  require(plan.report["plan"].get<std::string>() == expected_plan,
          "native hard reset plan changed; inspect the plan and retry");
  require(plan.report["ready"].get<bool>(), "native hard reset is blocked; inspect native reset-plan --hard");
  const auto refs = store.refs();
  require(lookup(refs, native_revision_reference(store, std::string(branch))) == std::optional{ObjectKey{Kind::revision, plan.head}} &&
              lookup(refs, native_staging_reference(store, std::string(branch))) == plan.index &&
              lookup(refs, store.native_workspace_reference(head_name)) == plan.selection &&
              lookup(refs, plan.materialization.reference) == plan.materialization.root,
          "native checkout references changed");
  if (!plan.report["changed"].get<bool>()) {
    auto result = plan.report;
    result["state"] = "unchanged";
    result["pending"] = false;
    result["archive"] = Json{};
    result["recovery_revision"] = Json{};
    return result;
  }
  auto ingest = store.settings().ingest_options();
  ingest.stop = options.stop;
  ingest.progress = options.progress;
  for (const auto &file : plan.working) {
    auto &entry = plan.files.at(file.relative);
    if (!entry.manifest.empty())
      continue;
    const auto imported =
        ingest_native_file(store, file, ingest, "working file changed during native hard reset");
    const auto metadata = store.metadata(Kind::manifest, imported["manifest"].get<std::string>());
    require(metadata["size"].get<std::uint64_t>() == entry.content.size &&
                metadata["file_hash"].get<std::string>() == entry.content.hash,
            "working file changed during native hard reset");
    entry.manifest = imported["manifest"].get<std::string>();
  }
  Json files = Json::object();
  auto &entries = files.get_ref<Json::object_t &>();
  entries.reserve(plan.files.size());
  for (const auto &[path, entry] : plan.files)
    entries.emplace_back(path, Json{{"manifest", entry.manifest}, {"executable", entry.executable}});
  Json parents = Json::array({plan.head});
  if (plan.index && plan.index->id != plan.head)
    parents.push_back(plan.index->id);
  const Json recovery{{"version", 1},
                      {"parents", parents},
                      {"message", "Syrnck hard reset recovery v1: " + std::string(branch)},
                      {"timestamp", 0},
                      {"files", std::move(files)}};
  throw_if_cancelled(options.stop);
  const auto source = store.put(Kind::revision, bytes(recovery.dump())).first;
  Record record;
  record.context = store.native_workspace_name();
  record.hard_reset = true;
  record.freeze_materialization(plan.materialization.policy);
  record.namespace_transition = plan.report["workspace"].value("namespace_transition", false);
  record.source_directories = std::move(plan.directories);
  record.workspace = fs::canonical(workspace);
  record.nonce = hex(random_bytes(32));
  record.branch = branch;
  record.source = source.id;
  record.target = target;
  record.original_head = ObjectKey{Kind::revision, plan.head};
  record.original_index = plan.index;
  record.previous_head = plan.selection;
  record.id = hash(bytes(record.identity().dump()));
  if (present(store.root() / "checkouts"))
    directory(store.root() / "checkouts", false);
  require(!present(record.data(store)), "native checkout transaction directory already exists");
  record.key = record.save(store);
  std::vector<RefUpdate> updates{{store.native_workspace_reference(active_name), {}, record.key},
                                 {store.native_workspace_reference(head_name), record.previous_head, record.previous_head},
                                 {record.branch_reference(), record.original_head, record.original_head},
                                 {record.index_reference(), record.original_index, record.original_index},
                                 {record.pin("target"), {}, record.destination()},
                                 {record.pin("source"), {}, source}, record.materialization_guard()};
  if (publisher) {
    const auto extra = publisher(source.id, plan);
    updates.insert(updates.end(), extra.begin(), extra.end());
  }
  require(store.compare_update_refs(updates, options), "native checkout references changed");
  auto result = Transaction(store, std::move(record), options).run(false, options);
  result["operation"] = "reset";
  result["mode"] = "hard";
  result["previous_revision"] = expected_head;
  result["previous_index"] = plan.index ? Json(plan.index->id) : Json{};
  result["recovery_revision"] = source.id;
  result["plan"] = expected_plan;
  return result;
}

Json native_hard_reset(const Store &store, const fs::path &workspace, std::string_view branch,
                       std::string_view expected_head, std::string_view target,
                       std::string_view expected_plan, OperationOptions options) {
  return capture_native_hard_reset(store, workspace, branch, expected_head, target, expected_plan, {}, {},
                                   options);
}

Json checkout_native_stash(const Store &store, const fs::path &workspace, std::string_view branch,
                           const NativeStashApplyPlan &planned, bool pop, OperationOptions options) {
  throw_if_cancelled(options.stop);
  require_no_native_checkout(store);
  require(read_native_materialization(store).root == planned.materialization.root,
          "native checkout references changed");
  const auto plan = plan_native_checkout_files(store, workspace, branch, planned.files, options, true);
  require(plan["ready"] == true && plan["source"].get<std::string>() == planned.head,
          "native stash workspace changed; refresh the plan");
  Record record;
  record.context = store.native_workspace_name();
  record.restore_worktree = true;
  record.freeze_materialization(planned.materialization.policy);
  record.workspace = fs::canonical(workspace);
  record.nonce = hex(random_bytes(32));
  record.branch = branch;
  record.source = planned.head;
  record.target = planned.target;
  record.namespace_transition = plan.value("namespace_transition", false);
  record.original_head = ObjectKey{Kind::revision, planned.head};
  record.previous_head = planned.selection;
  if (planned.index)
    record.target_index = ObjectKey{Kind::revision, *planned.index};
  record.stash_id = planned.saved.id;
  record.stash_work = planned.saved.work;
  record.stash_record = planned.saved.record.id;
  record.pop_stash = pop;
  record.id = hash(bytes(record.identity().dump()));
  if (present(store.root() / "checkouts"))
    directory(store.root() / "checkouts", false);
  require(!present(record.data(store)), "native checkout transaction directory already exists");
  record.key = record.save(store);
  std::vector<RefUpdate> updates{{store.native_workspace_reference(active_name), {}, record.key},
                                 {store.native_workspace_reference(head_name), record.previous_head, record.previous_head},
                                 {record.branch_reference(), record.original_head, record.original_head},
                                 {record.index_reference(), {}, {}},
                                 {record.pin("target"), {}, record.destination()},
                                 {record.pin("source"), {}, record.origin()}, record.materialization_guard()};
  const auto guard = native_stash_guard(store, planned.saved, false);
  updates.insert(updates.end(), guard.begin(), guard.end());
  require(store.compare_update_refs(updates, options), "native checkout references changed");
  return Transaction(store, std::move(record), options).run(false, options);
}

Json set_native_materialization(const Store &store, const fs::path &workspace,
    const std::optional<std::vector<std::string>> &prefixes, std::string_view expected_head,
    std::string_view expected_plan, OperationOptions options) {
  throw_if_cancelled(options.stop);
  validate_id(expected_head);
  validate_id(expected_plan);
  require_no_native_checkout(store);
  const auto plan = prepare_native_materialization(store, workspace, prefixes, true, options);
  require(plan.head == expected_head && plan.report["plan"].get<std::string>() == expected_plan,
          "native materialization plan changed; inspect the plan and retry");
  require(plan.report["ready"].get<bool>(), "native materialization is blocked; inspect the plan");
  const auto refs = store.refs();
  require(store.native_workspace_name() == plan.context &&
              lookup(refs, plan.source.reference) == plan.source.root &&
              lookup(refs, native_revision_reference(store, plan.branch)) == ObjectKey{Kind::revision, plan.head} &&
              lookup(refs, native_staging_reference(store, plan.branch)) == plan.index &&
              lookup(refs, store.native_workspace_reference(head_name)) == plan.selection,
          "native checkout references changed");
  if (!plan.report["changed"].get<bool>()) {
    auto result = plan.report;
    result["state"] = "unchanged";
    result["pending"] = false;
    return result;
  }
  store.require_features({"materialization-transitions-v1", "sparse-workspaces-v1"}, options);
  Record record;
  record.materialization_transition = true;
  record.context = plan.context;
  record.branch = plan.branch;
  record.source = plan.head;
  record.target = plan.head;
  record.original_head = ObjectKey{Kind::revision, plan.head};
  record.previous_head = plan.selection;
  record.source_materialization = plan.source.policy;
  record.target_materialization = plan.target;
  record.namespace_transition = plan.report["workspace"].value("namespace_transition", false);
  record.workspace = fs::canonical(workspace);
  record.nonce = hex(random_bytes(32));
  record.id = hash(bytes(record.identity().dump()));
  for (const auto *policy : {&record.source_materialization, &record.target_materialization})
    if (!policy->complete())
      require(store.put(Kind::chunk, bytes(policy->encode())).first.id == policy->identity(),
              "invalid native materialization");
  if (present(store.root() / "checkouts")) directory(store.root() / "checkouts", false);
  require(!present(record.data(store)), "native checkout transaction directory already exists");
  record.key = record.save(store);
  const std::vector<RefUpdate> updates{
      {record.active_reference(), {}, record.key},
      {record.branch_reference(), record.origin(), record.origin()},
      {record.index_reference(), {}, {}}, {record.head_reference(), record.previous_head, record.previous_head},
      {record.pin("source"), {}, record.origin()}, {record.pin("target"), {}, record.destination()},
      {record.pin("source-policy"), {}, record.policy_root(false)},
      {record.pin("target-policy"), {}, record.policy_root(true)}, record.materialization_guard()};
  require(store.compare_update_refs(updates, options), "native checkout references changed");
  auto result = Transaction(store, std::move(record), options).run(false, options);
  result["operation"] = "materialization";
  result["plan"] = expected_plan;
  return result;
}

} // namespace syrnck
