#include "native_workspace.hpp"
#include <syrnck/backup.hpp>
#include <limits>
#include <blake3.h>
namespace syrnck {
namespace {
constexpr std::string_view marker_name = "backup.json";
std::vector<ObjectKey> roots(const Refs &refs) {
  std::set<ObjectKey> unique;
  for (const auto &[name, key] : refs) {
    (void)name;
    unique.insert(key);
  }
  return {unique.begin(), unique.end()};
}
void quiescent(const Store &store, OperationOptions options) {
  require_no_native_checkout(store);
  for (const auto &session : store.transfer_sessions(options.stop))
    require(session.state == "complete", "backup requires completed or retired transfer sessions");
  const auto now = unix_seconds();
  for (const auto &lease : store.upload_leases(options.stop))
    require(lease.state != "active" || lease.expires_at <= now, "backup requires quiescent upload leases");
  for (const auto &lease : store.read_leases(options.stop))
    require(lease.state != "active" || lease.expires_at <= now, "backup requires quiescent read leases");
}
Refs portable_refs(const Refs &refs, Json &excluded) {
  auto result = refs;
  for (auto it = result.begin(); it != result.end();)
    if (it->first == "workspace/HEAD" || it->first.starts_with("checkouts/receipts/")) {
      excluded.push_back(it->first);
      it = result.erase(it);
    } else
      ++it;
  return result;
}
std::string refs_hash(const Refs &refs) {
  return hash(bytes(encode_refs(refs).dump()));
}
Json marker(const Store &store) {
  const auto path = store.root() / marker_name;
  const auto status = fs::symlink_status(path);
  require(fs::is_regular_file(status) && !native_link_like(path, status), "invalid backup descriptor");
  const auto data = read_bytes(path, max_object_size);
  const auto value = parse_json(data);
  require(value.is_object() && value.size() == 9 && value.contains("version") &&
              value["version"].is_number_unsigned() && value["version"] == 1 && value.contains("type") &&
              value["type"] == "store-backup" && value.contains("created_at") &&
              value["created_at"].is_number_unsigned() && value.contains("refs_hash") &&
              value["refs_hash"].is_string() && value.contains("settings_hash") &&
              value["settings_hash"].is_string() && value.contains("object_count") &&
              value["object_count"].is_number_unsigned() && value.contains("logical_bytes") &&
              value["logical_bytes"].is_number_unsigned() && value.contains("excluded_refs") &&
              value["excluded_refs"].is_array() && value.contains("source_encrypted") &&
              value["source_encrypted"].is_boolean() && value.dump() == text(data),
          "invalid backup descriptor");
  validate_id(value["refs_hash"].get<std::string>());
  validate_id(value["settings_hash"].get<std::string>());
  std::set<std::string> names;
  for (const auto &name : value["excluded_refs"]) {
    require(name.is_string(), "invalid backup descriptor");
    const auto ref = name.get<std::string>();
    validate_ref(ref);
    require((ref == "workspace/HEAD" || ref.starts_with("checkouts/receipts/")) && names.insert(ref).second,
            "invalid backup descriptor");
  }
  require(value["refs_hash"] == refs_hash(store.refs()) &&
              value["settings_hash"] == hash(bytes(store.settings().json().dump())),
          "backup references or settings changed");
  return value;
}
bool inside(const fs::path &path, const fs::path &root) {
  auto p = path.begin();
  for (const auto &part : root) {
    if (p == path.end() || *p != part)
      return false;
    ++p;
  }
  return true;
}
fs::path destination_path(const Store &source, const fs::path &destination) {
  auto target = fs::absolute(destination).lexically_normal();
  if (target.filename().empty())
    target = target.parent_path();
  require(!target.filename().empty(), "invalid backup destination");
  target = fs::canonical(target.parent_path()) / target.filename();
  require(!inside(target, source.root()), "backup destination must be outside the source store");
  return target;
}
Json copy(const Store &source, const fs::path &destination, std::optional<Key> key, bool backup,
          OperationOptions options) {
  throw_if_cancelled(options.stop);
  require(!source.encrypted() || key.has_value(), "encrypted backups require a destination encryption key");
  quiescent(source, options);
  const auto target = destination_path(source, destination);
  const auto original = source.refs();
  Json excluded = Json::array();
  const auto refs = portable_refs(original, excluded);
  const auto settings = source.settings();
  const auto objects = source.closure(roots(refs), true, options.stop, options.progress);
  auto descriptor = Json{{"version", 1},
                         {"type", "store-backup"},
                         {"created_at", unix_seconds()},
                         {"refs_hash", refs_hash(refs)},
                         {"settings_hash", hash(bytes(settings.json().dump()))},
                         {"object_count", objects.size()},
                         {"logical_bytes", 0},
                         {"source_encrypted", source.encrypted()},
                         {"excluded_refs", excluded}};
  std::uint64_t total = 0, completed = 0;
  Json report;
  atomic_directory(
      target,
      [&](const fs::path &temporary) {
        Store output(temporary, key, true);
        output.set_settings(settings);
        for (const auto &object : objects) {
          report_progress(options, {ProgressPhase::transferring, object.id, completed, objects.size()});
          const auto data = source.get(object);
          require(data.size() <= std::numeric_limits<std::uint64_t>::max() - total,
                  "backup logical size overflow");
          output.import(object, data);
          total += data.size();
          ++completed;
        }
        std::vector<RefUpdate> updates;
        for (const auto &[name, root] : refs)
          updates.push_back({name, {}, root});
        require(output.compare_update_refs(updates, {options.stop}),
                "backup references changed during creation");
        require(output.fsck({options.stop}) == objects.size(),
                "backup object inventory changed during creation");
        descriptor["logical_bytes"] = total;
        if (backup)
          atomic_write(temporary / marker_name, bytes(descriptor.dump()), false, options.stop);
        report = Json{{"version", 1},
                      {"state", backup ? "created" : "restored"},
                      {"destination", utf8(target)},
                      {"references", refs.size()},
                      {"objects", objects.size()},
                      {"logical_bytes", total},
                      {"encrypted", key.has_value()},
                      {"excluded_refs", excluded},
                      {"snapshot", hash(bytes(descriptor.dump()))}};
      },
      [&] {
        require(source.refs() == original && source.settings().json() == settings.json(),
                "backup source changed before publication");
        report_progress(options, {ProgressPhase::publishing, utf8(target), completed, objects.size(), true});
      },
      options.stop);
  return report;
}
} // namespace
Json create_backup(const Store &store, const fs::path &destination, std::optional<Key> key,
                   OperationOptions options) {
  return copy(store, destination, key, true, options);
}
Json verify_backup(const Store &store, OperationOptions options) {
  throw_if_cancelled(options.stop);
  const auto descriptor = marker(store);
  quiescent(store, options);
  const auto refs = store.refs();
  const auto objects = store.closure(roots(refs), true, options.stop, options.progress);
  require(objects.size() == descriptor["object_count"].get<std::uint64_t>(), "backup object count mismatch");
  require(store.fsck(options) == objects.size(), "backup object count mismatch");
  std::uint64_t total = 0;
  for (const auto &key : objects) {
    throw_if_cancelled(options.stop);
    const auto data = store.get(key);
    require(data.size() <= std::numeric_limits<std::uint64_t>::max() - total, "backup logical size overflow");
    total += data.size();
  }
  require(total == descriptor["logical_bytes"].get<std::uint64_t>(), "backup logical byte count mismatch");
  return Json{{"version", 1},
              {"state", "verified"},
              {"snapshot", hash(bytes(descriptor.dump()))},
              {"objects", objects.size()},
              {"references", refs.size()},
              {"logical_bytes", total},
              {"encrypted", store.encrypted()},
              {"excluded_refs", descriptor["excluded_refs"]}};
}
Json restore_backup(const Store &store, const fs::path &destination, std::optional<Key> key,
                    OperationOptions options) {
  (void)verify_backup(store, options);
  return copy(store, destination, key, false, options);
}
Json repair_from_backup(const Store &destination, const Store &backup, bool apply, OperationOptions options) {
  throw_if_cancelled(options.stop);
  require(destination.root() != backup.root(), "backup repair requires distinct stores");
  quiescent(destination, options);
  (void)verify_backup(backup, options);
  const auto refs = destination.refs();
  Json excluded = Json::array();
  const auto portable = portable_refs(refs, excluded);
  std::set<ObjectKey> selected, damaged, seen;
  std::size_t budget = 4096;
  auto record = [&](std::set<ObjectKey> &set, const ObjectKey &key) {
    if (set.insert(key).second) {
      const auto cost = key.id.size() + 128;
      require(cost <= max_object_size - budget, "backup repair plan exceeds 16 MiB");
      budget += cost;
    }
  };
  auto load = [&](const ObjectKey &key) {
    throw_if_cancelled(options.stop);
    try {
      return destination.get(key);
    } catch (const MissingObject &) {
      record(selected, key);
    } catch (const Error &) {
      record(damaged, key);
    }
    return backup.get(key);
  };
  auto pending = roots(portable);
  while (!pending.empty()) {
    auto key = std::move(pending.back());
    pending.pop_back();
    if (!seen.insert(key).second)
      continue;
    report_progress(options, {ProgressPhase::verifying, key.id, seen.size()});
    const auto data = load(key);
    auto children = dependencies(key.kind, data);
    pending.insert(pending.end(), children.begin(), children.end());
    if (key.kind == Kind::manifest) {
      const auto manifest = parse_json(data);
      blake3_hasher whole;
      blake3_hasher_init(&whole);
      for (const auto &block : manifest["blocks"]) {
        const auto bytes = load({Kind::chunk, block["id"].get<std::string>()});
        require(bytes.size() == block["size"].get<std::uint64_t>(), "backup repair block size mismatch");
        blake3_hasher_update(&whole, bytes.data(), bytes.size());
      }
      Key digest{};
      blake3_hasher_finalize(&whole, digest.data(), digest.size());
      require(hex(digest) == manifest["file_hash"].get<std::string>(), "backup repair file hash mismatch");
    }
  }
  Json missing = Json::array(), blockers = Json::array();
  for (const auto &key : selected)
    missing.push_back(key_json(key));
  for (const auto &key : damaged)
    blockers.push_back(key_json(key));
  auto report = Json{{"version", 1},
                     {"operation", "repair"},
                     {"applied", apply},
                     {"ready", blockers.empty()},
                     {"references_hash", refs_hash(refs)},
                     {"missing", std::move(missing)},
                     {"corrupt", std::move(blockers)},
                     {"repaired", 0}};
  if (apply) {
    require(report["ready"].get<bool>(), "backup repair is blocked by corrupt existing objects");
    require(destination.refs() == refs, "backup repair references changed; refresh the plan");
    std::uint64_t repaired = 0;
    for (const auto &key : selected) {
      report_progress(options, {ProgressPhase::transferring, key.id, repaired, selected.size()});
      destination.import(key, backup.get(key));
      ++repaired;
    }
    (void)destination.closure(roots(portable), true, options.stop, options.progress);
    report["repaired"] = repaired;
  }
  return report;
}
} // namespace syrnck
