#include "native_stash.hpp"
#include "native_hard_reset.hpp"
#include <algorithm>
#include <limits>
namespace syrnck {
namespace {
std::optional<ObjectKey> lookup(const Refs &refs, const std::string &name) {
  const auto found = refs.find(name);
  return found == refs.end() ? std::optional<ObjectKey>{} : std::optional{found->second};
}
struct Clock {
  std::optional<ObjectKey> key;
  std::uint64_t value = 0;
};
Clock clock(const Store &store, const Refs &refs) {
  Clock result;
  result.key = lookup(refs, "stash/clock");
  if (result.key) {
    require(result.key->kind == Kind::chunk, "invalid native stash clock");
    const auto data = store.get(*result.key);
    const auto value = parse_json(data);
    require(value.is_object() && value.size() == 3 && value.value("version", 0) == 1 &&
                value.value("type", "") == "native-stash-clock" && value.contains("value") &&
                value["value"].is_number_unsigned(),
            "invalid native stash clock");
    result.value = value["value"].get<std::uint64_t>();
    require(result.value > 0 &&
                Json{{"version", 1}, {"type", "native-stash-clock"}, {"value", result.value}}.dump() ==
                    text(data),
            "invalid native stash clock");
  }
  return result;
}
Clock clock(const Store &store) {
  return clock(store, store.refs());
}
Json summary(const NativeStashEntry &entry) {
  auto message = entry.message;
  auto end = std::min<std::size_t>(256, message.size());
  if (end < message.size())
    while (end && (static_cast<unsigned char>(message[end]) & 0xc0) == 0x80)
      --end;
  return Json{{"id", entry.id},
              {"record", entry.record.id},
              {"generation", entry.generation},
              {"branch", entry.branch},
              {"base", entry.base},
              {"work", entry.work},
              {"index", entry.index ? Json(*entry.index) : Json{}},
              {"created_at", entry.created_at},
              {"message", message.substr(0, end)},
              {"message_truncated", end < message.size()}};
}
void verify(const Store &store, const NativeStashEntry &entry, OperationOptions options) {
  (void)store.closure({{Kind::revision, entry.work}}, true, options.stop, options.progress);
}
} // namespace
Json NativeStashEntry::json() const {
  return Json{{"version", native_target_is_detached(branch) ? 2 : 1}, {"type", "native-stash"},
              {"id", id},           {"generation", generation},
              {"branch", branch},   {"base", base},
              {"work", work},       {"index", index ? Json(*index) : Json{}},
              {"message", message}, {"created_at", created_at}};
}
static NativeStashEntry read_stash_from_refs(const Store &store, std::string_view id, bool archived,
                                             OperationOptions options, const Refs &refs,
                                             std::uint64_t maximum, bool inspect_snapshot = true) {
  throw_if_cancelled(options.stop);
  validate_id(id);
  NativeStashEntry entry;
  entry.id = id;
  const auto key = lookup(refs, archived ? entry.archive_name() : entry.name());
  require(key && key->kind == Kind::chunk, "unknown native stash");
  entry.record = *key;
  const auto data = store.get(*key);
  const auto value = parse_json(data);
  require(value.is_object() && value.size() == 10 &&
              (value.value("version", 0) == 1 || value.value("version", 0) == 2) &&
              value.value("type", "") == "native-stash" && value.at("id") == Json(id) &&
              value.at("generation").is_number_unsigned() && value.at("created_at").is_number_unsigned(),
          "invalid native stash record");
  entry.branch = value.at("branch").get<std::string>();
  entry.base = value.at("base").get<std::string>();
  entry.work = value.at("work").get<std::string>();
  entry.message = value.at("message").get<std::string>();
  entry.generation = value.at("generation").get<std::uint64_t>();
  entry.created_at = value.at("created_at").get<std::uint64_t>();
  if (!value.at("index").is_null())
    entry.index = value.at("index").get<std::string>();
  validate_ref(native_revision_reference(entry.branch));
  validate_ref(native_staging_reference(entry.branch));
  validate_id(entry.base);
  validate_id(entry.work);
  if (entry.index)
    validate_id(*entry.index);
  require(entry.message.size() <= 1024 * 1024 && entry.generation > 0 && entry.generation <= maximum &&
              entry.created_at <= static_cast<std::uint64_t>(INT64_MAX) && entry.json().dump() == text(data),
          "invalid native stash record");
  require(lookup(refs, archived ? entry.archive_work() : entry.work_name()) ==
              ObjectKey{Kind::revision, entry.work},
          "native stash protection reference changed");
  if (!inspect_snapshot)
    return entry;
  const auto snapshot = store.metadata(Kind::revision, entry.work);
  Json parents = Json::array({entry.base});
  if (entry.index && *entry.index != entry.base)
    parents.push_back(*entry.index);
  require(snapshot["parents"] == parents && snapshot["timestamp"] == 0 &&
              snapshot["message"] == "Syrnck hard reset recovery v1: " + entry.branch,
          "invalid native stash working snapshot");
  if (entry.index) {
    const auto index = store.metadata(Kind::revision, *entry.index);
    require(index["parents"].size() <= 1 && index["timestamp"] == 0 &&
                index["message"] == "Syrnck staging v1: " + entry.branch,
            "invalid native stash staging snapshot");
  }
  return entry;
}
NativeStashEntry read_native_stash(const Store &store, std::string_view id, bool archived,
                                   OperationOptions options) {
  const auto refs = store.refs();
  return read_stash_from_refs(store, id, archived, options, refs, clock(store, refs).value);
}
std::vector<RefUpdate> native_stash_guard(const Store &store, const NativeStashEntry &entry, bool remove) {
  const ObjectKey work{Kind::revision, entry.work};
  std::vector<RefUpdate> updates{
      {entry.name(), entry.record, remove ? std::optional<ObjectKey>{} : entry.record},
      {entry.work_name(), work, remove ? std::optional<ObjectKey>{} : work}};
  if (remove) {
    const auto refs = store.refs();
    const auto metadata = lookup(refs, entry.archive_name()), previous = lookup(refs, entry.archive_work());
    require((!metadata || *metadata == entry.record) && (!previous || *previous == work),
            "native stash archive reference changed");
    updates.push_back({entry.archive_name(), metadata, entry.record});
    updates.push_back({entry.archive_work(), previous, work});
  }
  return updates;
}
Json NativeStash::save(const fs::path &directory, std::string_view branch, std::string_view message,
                       NativeStashSaveOptions settings, OperationOptions options) const {
  throw_if_cancelled(options.stop);
  require_no_native_checkout(store_);
  require(message.size() <= 1024 * 1024, "native stash message exceeds 1 MiB");
  try {
    (void)Json(message).dump();
  } catch (const nlohmann::json::exception &) {
    throw Error("invalid native stash message");
  }
  const auto counter = clock(store_);
  require(counter.value < UINT64_MAX, "native stash generation overflow");
  const auto active = list({}, 1000, options);
  require(active["total"].get<std::size_t>() < 4096, "native stash count limit exceeded");
  std::set<std::string> additional;
  if (settings.include_untracked)
    visit_native_files(
        directory, store_,
        [&](const NativeFile &file) { additional.insert(portable_fold(file.relative)); }, options);
  const auto selected = store_.native_head();
  require(selected["revision"].is_string(), "native stash requires an existing selected head");
  const auto head = selected["revision"].get<std::string>();
  const auto plan = prepare_native_hard_reset(store_, directory, branch, head, false, options, additional);
  NativeStashEntry entry;
  entry.id = hex(random_bytes(32));
  entry.branch = branch;
  entry.base = head;
  entry.message = message;
  entry.generation = counter.value + 1;
  entry.created_at = unix_seconds();
  auto result = capture_native_hard_reset(
      store_, directory, branch, head, head, plan.report["plan"].get<std::string>(), additional,
      [&](std::string_view work, const NativeHardResetPlan &captured) {
        entry.work = work;
        if (captured.index)
          entry.index = captured.index->id;
        entry.record = store_.put(Kind::chunk, bytes(entry.json().dump())).first;
        const auto next_clock =
            store_
                .put(Kind::chunk,
                     bytes(Json{{"version", 1}, {"type", "native-stash-clock"}, {"value", entry.generation}}
                               .dump()))
                .first;
        return std::vector<RefUpdate>{{entry.name(), {}, entry.record},
                                      {entry.work_name(), {}, ObjectKey{Kind::revision, entry.work}},
                                      {"stash/clock", counter.key, next_clock}};
      },
      options);
  result["operation"] = "stash-save";
  result["stash"] = entry.work.empty() ? Json{} : summary(entry);
  return result;
}
Json NativeStash::list(const std::optional<std::uint64_t> &before, std::size_t limit,
                       OperationOptions options) const {
  throw_if_cancelled(options.stop);
  require(limit >= 1 && limit <= 1000, "native stash page limit must be in 1..1000");
  const auto refs = store_.refs();
  const auto maximum = clock(store_, refs).value;
  std::vector<std::pair<std::uint64_t, Json>> entries;
  std::set<std::uint64_t> generations;
  for (auto at = refs.lower_bound("stash/"); at != refs.end() && at->first.starts_with("stash/"); ++at) {
    throw_if_cancelled(options.stop);
    if (at->first == "stash/clock")
      continue;
    require((at->first.size() == 77 && at->first.ends_with("/record")) ||
                (at->first.size() == 75 && at->first.ends_with("/work")),
            "invalid native stash reference");
    if (!at->first.ends_with("/record")) {
      const auto id = at->first.substr(6, 64);
      require(refs.contains("stash/" + id + "/record"), "orphan native stash working reference");
      continue;
    }
    require(entries.size() < 4096, "native stash count limit exceeded");
    auto entry = read_stash_from_refs(store_, at->first.substr(6, 64), false, options, refs, maximum, false);
    require(generations.insert(entry.generation).second, "duplicate native stash generation");
    entries.emplace_back(entry.generation, summary(entry));
  }
  std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
  Json rows = Json::array();
  bool more = false;
  for (const auto &entry : entries) {
    if (before && entry.first >= *before)
      continue;
    if (rows.size() == limit) {
      more = true;
      break;
    }
    rows.push_back(entry.second);
  }
  return Json{{"version", 1},
              {"stashes", rows},
              {"total", entries.size()},
              {"has_more", more},
              {"next_before", more ? rows.back()["generation"] : Json{}}};
}
Json NativeStash::show(std::string_view id, bool verify_objects, OperationOptions options) const {
  const auto entry = read_native_stash(store_, id, false, options);
  if (verify_objects)
    verify(store_, entry, options);
  auto result = entry.json();
  result["record"] = entry.record.id;
  result["objects_verified"] = verify_objects;
  return result;
}
Json NativeStash::drop(std::string_view id, std::string_view expected, OperationOptions options) const {
  throw_if_cancelled(options.stop);
  validate_id(expected);
  require_no_native_checkout(store_);
  const auto entry = read_native_stash(store_, id, false, options);
  require(entry.record.id == expected, "native stash record changed; refresh the selection");
  verify(store_, entry, options);
  require(store_.compare_update_refs(native_stash_guard(store_, entry, true), options),
          "native stash references changed; retry operation");
  return Json{{"version", 1}, {"state", "archived"}, {"stash", summary(entry)}};
}
Json NativeStash::archived(std::string_view id, bool verify_objects, OperationOptions options) const {
  const auto entry = read_native_stash(store_, id, true, options);
  if (verify_objects)
    verify(store_, entry, options);
  auto result = entry.json();
  result["record"] = entry.record.id;
  result["objects_verified"] = verify_objects;
  return result;
}
Json NativeStash::restore(std::string_view id, OperationOptions options) const {
  throw_if_cancelled(options.stop);
  require_no_native_checkout(store_);
  const auto entry = read_native_stash(store_, id, true, options);
  verify(store_, entry, options);
  require(list({}, 1000, options)["total"].get<std::size_t>() < 4096, "native stash count limit exceeded");
  require(store_.compare_update_refs({{entry.name(), {}, entry.record},
                                      {entry.work_name(), {}, ObjectKey{Kind::revision, entry.work}},
                                      {entry.archive_name(), entry.record, entry.record},
                                      {entry.archive_work(), ObjectKey{Kind::revision, entry.work},
                                       ObjectKey{Kind::revision, entry.work}}},
                                     options),
          "native stash references changed; retry operation");
  return Json{{"version", 1}, {"state", "restored"}, {"stash", summary(entry)}};
}
} // namespace syrnck
