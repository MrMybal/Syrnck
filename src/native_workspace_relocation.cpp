#include "native_workspace_relocation.hpp"
#include "native_workspace.hpp"
#include "native_checkout_layout.hpp"
#include <syrnck/workspace.hpp>
#include <algorithm>
namespace syrnck {
namespace {
std::optional<ObjectKey> lookup(const Refs &refs, std::string_view name) {
  const auto at = refs.find(std::string(name));
  return at == refs.end() ? std::optional<ObjectKey>{} : std::optional{at->second};
}
ObjectKey chunk_key(const Json &value) { return {Kind::chunk, object_id(Kind::chunk, bytes(value.dump()))}; }
std::string refs_hash(const Refs &refs) { return hash(bytes(encode_refs(refs).dump())); }
std::string folded(const fs::path &path) { return portable_fold(utf8(path)); }
bool below(const fs::path &path, const fs::path &parent) {
  const auto value = folded(path), root = folded(parent);
  return value == root || (value.starts_with(root) && (root.back() == '/' || value[root.size()] == '/'));
}
NativeDirectoryIdentity identity(const fs::path &path) {
  const auto value = native_directory_identity(path);
  require(value.has_value(), "workspace directory identity is unavailable");
  return *value;
}
NativeDirectoryIdentity decode_identity(const Json &value) {
  require(value.is_object() && value.size() == 2 && value.at("volume").is_string() &&
      value.at("object").is_string(), "invalid native workspace relocation");
  NativeDirectoryIdentity result{value.at("volume").get<std::string>(), value.at("object").get<std::string>()};
  require(!result.volume.empty() && !result.object.empty(), "invalid native workspace relocation");
  return result;
}
bool present(const fs::path &path) { return fs::symlink_status(path).type() != fs::file_type::not_found; }
bool exact_leaf(const fs::path &path) {
  for (const auto &entry : fs::directory_iterator(path.parent_path()))
    if (entry.path().filename() == path.filename()) return true;
  return false;
}
struct Record {
  std::string name, nonce, references, plan;
  fs::path source, destination, bridge;
  NativeDirectoryIdentity directory, source_parent, destination_parent;
  Json registration, head;
  ObjectKey key{Kind::chunk, {}};
  std::string registration_name() const { return native_context_reference(name, "registration"); }
  std::string head_name() const { return native_context_reference(name, "workspace/HEAD"); }
  Json new_registration() const { auto value = registration; value["directory"] = utf8(destination); return value; }
  Json new_head() const { auto value = head; value["directory"] = utf8(destination); return value; }
  std::string receipt(bool abort) const { return std::string(native_relocation_archive) + key.id + (abort ? "/aborted" : "/completed"); }
  Json identity_value(const Store &store) const {
    return Json{{"version", 1}, {"type", "native-workspace-relocation-plan"}, {"store", utf8(store.root())},
        {"name", name}, {"source", utf8(source)}, {"destination", utf8(destination)},
        {"identity", directory.json()}, {"source_parent", source_parent.json()}, {"destination_parent", destination_parent.json()},
        {"registration", registration}, {"head", head}, {"refs_hash", references}};
  }
  std::string plan_id(const Store &store) const { return hash(bytes(identity_value(store).dump())); }
  Json json() const {
    return Json{{"version", 1}, {"type", "native-workspace-relocation"}, {"name", name}, {"nonce", nonce},
        {"source", utf8(source)}, {"destination", utf8(destination)}, {"bridge", utf8(bridge)},
        {"identity", directory.json()}, {"source_parent", source_parent.json()}, {"destination_parent", destination_parent.json()},
        {"registration", registration}, {"head", head}, {"refs_hash", references}, {"plan", plan}};
  }
};
Record read_record(const Store &store, const ObjectKey &key) {
  require(key.kind == Kind::chunk, "invalid native workspace relocation");
  const auto data = store.get(key);
  const auto value = parse_json(data);
  require(value.is_object() && value.size() == 14 && value.value("version", 0) == 1 &&
      value.value("type", "") == "native-workspace-relocation", "invalid native workspace relocation");
  Record result;
  result.key = key; result.name = value.at("name").get<std::string>(); validate_native_workspace_name(result.name);
  result.nonce = value.at("nonce").get<std::string>(); validate_id(result.nonce);
  result.references = value.at("refs_hash").get<std::string>(); validate_id(result.references);
  result.plan = value.at("plan").get<std::string>(); validate_id(result.plan);
  result.source = path_from_utf8(value.at("source").get<std::string>());
  result.destination = path_from_utf8(value.at("destination").get<std::string>());
  result.bridge = path_from_utf8(value.at("bridge").get<std::string>());
  for (const auto &path : {result.source, result.destination, result.bridge})
    require(path.is_absolute() && path == path.lexically_normal() && path != path.root_path() &&
        !below(path, store.root()) && !below(store.root(), path), "invalid native workspace relocation");
  require(result.source != result.destination && result.bridge != result.source && result.bridge != result.destination &&
      result.bridge == result.source.parent_path() /
      path_from_utf8(".syrnck-relocate-" + result.nonce), "invalid native workspace relocation");
  if (folded(result.source) != folded(result.destination))
    require(!below(result.source, result.destination) && !below(result.destination, result.source), "invalid native workspace relocation");
  result.directory = decode_identity(value.at("identity"));
  result.source_parent = decode_identity(value.at("source_parent"));
  result.destination_parent = decode_identity(value.at("destination_parent"));
  require(result.directory.volume == result.source_parent.volume && result.directory.volume == result.destination_parent.volume,
      "invalid native workspace relocation");
  result.registration = value.at("registration"); result.head = value.at("head");
  const auto &head = result.head;
  require(result.registration == Json{{"version", 1}, {"type", "native-workspace"}, {"name", result.name}, {"directory", utf8(result.source)}} &&
      head.is_object() && head.size() == 4 && head.value("type", "") == "native-head" && head.at("directory") == utf8(result.source) &&
      ((head.at("branch").is_string() && head.at("version") == 1) || (head.at("branch").is_null() && head.at("version") == 2)),
      "invalid native workspace relocation");
  if (head["branch"].is_string()) validate_ref(head["branch"].get<std::string>());
  require(result.plan_id(store) == result.plan && result.json().dump() == text(data), "invalid native workspace relocation");
  return result;
}
void verify_parents(const Record &record) {
  require(identity(record.source.parent_path()) == record.source_parent &&
      identity(record.destination.parent_path()) == record.destination_parent,
      "workspace relocation parent directory changed");
}
std::string phase(const Record &record) {
  verify_parents(record);
  const bool aliases = folded(record.source) == folded(record.destination);
  const auto at = [&](const fs::path &path, bool alias) {
    if (!present(path)) return false;
    require(native_directory_identity(path) == record.directory, "workspace relocation path is occupied or changed");
    if (exact_leaf(path)) return true;
    require(alias, "workspace relocation directory spelling changed");
    return false;
  };
  const bool source = at(record.source, aliases), bridge = at(record.bridge, false), destination = at(record.destination, aliases);
  require(static_cast<unsigned>(source) + static_cast<unsigned>(bridge) + static_cast<unsigned>(destination) == 1,
      "workspace relocation directory identity is ambiguous");
  return source ? "source" : bridge ? "bridge" : "destination";
}
void old_bindings(const Record &record, const Refs &refs) {
  require(lookup(refs, record.registration_name()) == chunk_key(record.registration) &&
      lookup(refs, record.head_name()) == chunk_key(record.head), "workspace relocation references changed");
}
void verify_destination_namespace(const Store &store, const Record &record) {
  const bool aliases = folded(record.source) == folded(record.destination);
  const auto rows = store.native_workspaces()["workspaces"];
  for (const auto &row : rows) {
    if (row["name"] == record.name && aliases) continue;
    const auto other = path_from_utf8(row["directory"].get<std::string>());
    require(!below(record.destination, other) && !below(other, record.destination), "native workspaces must not overlap");
  }
}
Json report(const Record &record, std::string_view state, bool pending) {
  return Json{{"version", 1}, {"operation", "workspace-relocate"}, {"name", record.name},
      {"source", utf8(record.source)}, {"destination", utf8(record.destination)}, {"bridge", utf8(record.bridge)},
      {"plan", record.plan}, {"transaction", record.key.id.empty() ? Json{} : Json(record.key.id)},
      {"state", state}, {"pending", pending}, {"ready", true}, {"preserves_all_files", true}};
}
Record prepare(const Store &store, std::string_view name, const fs::path &destination, OperationOptions options) {
  throw_if_cancelled(options.stop);
  require(store.native_workspace_name().empty(), "workspace relocation requires the primary context");
  require_no_native_checkout(store);
  const auto context = read_native_workspace_context(store, name);
  const auto refs = store.refs();
  Record result; result.name = name; result.source = context.directory;
  require(exact_leaf(result.source), "workspace relocation directory spelling changed");
  auto target = fs::absolute(destination).lexically_normal();
  require(target != target.root_path(), "native workspaces must not overlap");
  result.destination = fs::canonical(target.parent_path()) / target.filename();
  require(result.source != result.destination, "workspace relocation destination is unchanged");
  result.directory = identity(result.source);
  result.source_parent = identity(result.source.parent_path()); result.destination_parent = identity(result.destination.parent_path());
  require(result.directory.volume == result.source_parent.volume && result.directory.volume == result.destination_parent.volume,
      "workspace relocation requires one filesystem");
  const bool aliases = folded(result.source) == folded(result.destination);
  if (present(result.destination))
    require(aliases && native_directory_identity(result.destination) == result.directory && !exact_leaf(result.destination),
        "workspace relocation destination already exists");
  const auto workspaces = store.native_workspaces()["workspaces"];
  for (const auto &row : workspaces) {
    throw_if_cancelled(options.stop);
    const auto other = path_from_utf8(row["directory"].get<std::string>());
    if (row["name"] == Json(name) && aliases) continue;
    require(!below(result.destination, other) && !below(other, result.destination), "native workspaces must not overlap");
  }
  require(!below(result.destination, store.root()) && !below(store.root(), result.destination), "native workspaces must not overlap");
  const auto registration = lookup(refs, result.registration_name()), head = lookup(refs, result.head_name());
  require(registration && head && registration->kind == Kind::chunk && head->kind == Kind::chunk,
      "invalid native workspace registration");
  result.registration = parse_json(store.get(*registration)); result.head = parse_json(store.get(*head));
  result.references = refs_hash(refs); result.plan = result.plan_id(store);
  auto prospective = refs;
  prospective.emplace(std::string(native_relocation_active), ObjectKey{Kind::chunk, std::string(64, '0')});
  require(encode_refs(prospective).dump().size() + 128 <= max_object_size, "reference document exceeds 16 MiB");
  return result;
}
Refs base_refs(const Store &store, const Record &record) {
  auto refs = store.refs();
  require(lookup(refs, native_relocation_active) == record.key, "workspace relocation references changed");
  refs.erase(std::string(native_relocation_active));
  require(refs_hash(refs) == record.references, "workspace relocation references changed");
  old_bindings(record, refs);
  return refs;
}
} // namespace
void validate_native_workspace_relocation(const Store &store, const Refs &before, const Refs &after) {
  const auto old = lookup(before, native_relocation_active), next = lookup(after, native_relocation_active);
  if (!old && !next) {
    for (const auto &[name, key] : before) {
      if (!name.starts_with("workspaces/")) continue;
      const auto slash = name.find('/', 11);
      if (slash == name.npos || name.substr(slash + 1) != "registration") continue;
      const auto value = lookup(after, name);
      require(!value || *value == key, "live workspace relocation requires a recovery transaction");
    }
    return;
  }
  const auto features = store.required_features();
  require(std::binary_search(features.begin(), features.end(), "workspace-relocation-v1"),
      "workspace relocation requires a supported reader feature");
  if (!old) {
    const auto record = read_record(store, *next);
    require(refs_hash(before) == record.references && phase(record) == "source", "workspace relocation plan changed");
    old_bindings(record, before);
    verify_destination_namespace(store, record);
    require(std::none_of(before.begin(), before.end(), [](const auto &entry) { return native_checkout_active_reference(entry.first); }),
        "native checkout is pending; run native checkout-recover");
    auto expected = before; expected.emplace(std::string(native_relocation_active), *next);
    require(after == expected, "workspace relocation references changed");
    return;
  }
  const auto record = read_record(store, *old);
  auto base = before; base.erase(std::string(native_relocation_active));
  require(refs_hash(base) == record.references, "workspace relocation references changed");
  old_bindings(record, base);
  if (before == after) return;
  require(!next, "native workspace relocation is pending; run native workspace-relocation-recover");
  const auto state = phase(record);
  const bool abort = state == "source";
  require(abort || state == "destination", "workspace relocation has not finished moving files");
  if (!abort) {
    base[record.registration_name()] = chunk_key(record.new_registration());
    base[record.head_name()] = chunk_key(record.new_head());
  }
  require(!base.contains(record.receipt(abort)), "workspace relocation receipt already exists");
  base.emplace(record.receipt(abort), *old);
  require(after == base, "native workspace relocation is pending; run native workspace-relocation-recover");
}
Json plan_native_workspace_relocation(const Store &store, std::string_view name, const fs::path &destination, OperationOptions options) {
  const auto record = prepare(store, name, destination, options);
  auto result = report(record, "planned", false); result.erase("bridge");
  report_progress(options, {ProgressPhase::planning, "workspace-relocation", {}, {}, true});
  return result;
}
Json native_workspace_relocation_status(const Store &store, OperationOptions options) {
  throw_if_cancelled(options.stop);
  const auto active = lookup(store.refs(), native_relocation_active);
  if (!active) return Json{{"version", 1}, {"pending", false}, {"state", "none"}};
  const auto record = read_record(store, *active);
  (void)base_refs(store, record);
  return report(record, phase(record), true);
}
Json recover_native_workspace_relocation(const Store &store, bool abort, OperationOptions options) {
  throw_if_cancelled(options.stop);
  require(store.native_workspace_name().empty(), "workspace relocation requires the primary context");
  const auto active = lookup(store.refs(), native_relocation_active);
  if (!active) return Json{{"version", 1}, {"pending", false}, {"state", "none"}};
  const auto record = read_record(store, *active);
  auto state = phase(record);
  while ((abort && state != "source") || (!abort && state != "destination")) {
    report_progress(options, {ProgressPhase::publishing, "workspace-relocation/move"});
    (void)base_refs(store, record); state = phase(record);
    if ((abort && state == "source") || (!abort && state == "destination")) break;
    if (state == "bridge") move_exclusive(record.bridge, abort ? record.source : record.destination);
    else move_exclusive(abort ? record.destination : record.source, record.bridge);
    state = phase(record);
  }
  if (!abort) {
    store.put(Kind::chunk, bytes(record.new_registration().dump()));
    store.put(Kind::chunk, bytes(record.new_head().dump()));
  }
  report_progress(options, {ProgressPhase::publishing, "workspace-relocation/finalize"});
  (void)base_refs(store, record);
  require(phase(record) == (abort ? "source" : "destination"), "workspace relocation path is occupied or changed");
  std::vector<RefUpdate> updates{{std::string(native_relocation_active), record.key, {}}, {record.receipt(abort), {}, record.key}};
  if (!abort) {
    updates.push_back({record.registration_name(), chunk_key(record.registration), chunk_key(record.new_registration())});
    updates.push_back({record.head_name(), chunk_key(record.head), chunk_key(record.new_head())});
  }
  require(store.compare_update_refs(updates, {options.stop}), "workspace relocation references changed");
  return report(record, abort ? "aborted" : "completed", false);
}
Json relocate_native_workspace(const Store &store, std::string_view name, const fs::path &destination,
    std::string_view expected_plan, OperationOptions options) {
  throw_if_cancelled(options.stop); validate_id(expected_plan);
  require(store.native_workspace_name().empty(), "workspace relocation requires the primary context");
  const auto requested = fs::absolute(destination).lexically_normal();
  const auto target = fs::canonical(requested.parent_path()) / requested.filename();
  const auto refs = store.refs();
  if (const auto active = lookup(refs, native_relocation_active)) {
    const auto pending = read_record(store, *active);
    require(pending.name == name && pending.destination == target && pending.plan == expected_plan, "workspace relocation plan changed");
    return recover_native_workspace_relocation(store, false, options);
  }
  // A completed command may be retried after losing its response. Historical
  // source paths need not remain vacant once publication has completed.
  const auto prefix = std::string(native_relocation_archive);
  for (auto entry = refs.lower_bound(prefix); entry != refs.end() && entry->first.starts_with(prefix); ++entry) {
    if (!entry->first.ends_with("/completed") || entry->second.kind != Kind::chunk) continue;
    const auto value = parse_json(store.get(entry->second));
    if (value.value("name", "") != name || value.value("plan", "") != expected_plan || value.value("destination", "") != utf8(target)) continue;
    const auto completed = read_record(store, entry->second);
    require(entry->first == completed.receipt(false) && lookup(refs, completed.registration_name()) == chunk_key(completed.new_registration()) &&
        lookup(refs, completed.head_name()) == chunk_key(completed.new_head()) && identity(target) == completed.directory && exact_leaf(target),
        "workspace relocation references changed");
    return report(completed, "completed", false);
  }
  auto record = prepare(store, name, destination, options);
  require(record.plan == expected_plan, "workspace relocation plan changed");
  store.require_features({"workspace-relocation-v1"}, options);
  record.nonce = hex(random_bytes(32)); record.bridge = record.source.parent_path() / path_from_utf8(".syrnck-relocate-" + record.nonce);
  require(!present(record.bridge), "workspace relocation destination already exists");
  record.key = store.put(Kind::chunk, bytes(record.json().dump())).first;
  auto prospective = store.refs(); prospective.emplace(std::string(native_relocation_active), record.key);
  require(encode_refs(prospective).dump().size() + 128 <= max_object_size, "reference document exceeds 16 MiB");
  report_progress(options, {ProgressPhase::publishing, "workspace-relocation/prepare"});
  require(prepare(store, name, destination, {options.stop}).plan == record.plan, "workspace relocation plan changed");
  require(store.compare_update_refs({{std::string(native_relocation_active), {}, record.key}}, {options.stop}),
      "workspace relocation references changed");
  return recover_native_workspace_relocation(store, false, options);
}
} // namespace syrnck
