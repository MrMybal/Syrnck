#include <syrnck/asset_locks.hpp>
#include "internal.hpp"
#include <algorithm>
#include <limits>
#include <utf8proc.h>
namespace syrnck {
namespace {
constexpr std::string_view clock_name = "asset-locks/clock", prefix = "asset-locks/paths/";
constexpr std::size_t max_locks = 4096, max_batch = 1024;
constexpr std::uint64_t max_time = 253402300799ULL;
bool tagged(const Json &value, std::string_view type, std::size_t fields) {
  return value.is_object() && value.size() == fields && value.contains("version") &&
         value["version"].is_number_unsigned() && value["version"] == 1 && value.contains("type") &&
         value["type"].is_string() && value["type"].get<std::string>() == type;
}
void owner_valid(std::string_view owner) {
  require(!owner.empty() && owner.size() <= 256 &&
              std::none_of(owner.begin(), owner.end(), [](unsigned char c) { return c < 32 || c == 127; }),
          "asset lock owner must be 1..256 UTF-8 bytes without control characters");
  (void)portable_fold(owner);
  for (std::size_t offset = 0; offset < owner.size();) {
    utf8proc_int32_t point = 0;
    const auto count = utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t *>(owner.data() + offset),
                                        static_cast<utf8proc_ssize_t>(owner.size() - offset), &point);
    require(count > 0 && utf8proc_category(point) != UTF8PROC_CATEGORY_CC,
            "asset lock owner must be 1..256 UTF-8 bytes without control characters");
    offset += static_cast<std::size_t>(count);
  }
}
std::string id(std::string_view path) {
  require(path.size() <= 4096, "asset lock path exceeds 4096 UTF-8 bytes");
  validate_path(path);
  return hash(bytes(portable_fold(path)));
}
struct Entry {
  std::string path, owner;
  std::uint64_t fence, created, expires;
  ObjectKey key;
  Json json() const {
    return Json{{"version", 1},   {"type", "asset-lock"},  {"path", path},         {"owner", owner},
                {"fence", fence}, {"created_at", created}, {"expires_at", expires}};
  }
};
struct State {
  std::optional<ObjectKey> counter;
  std::uint64_t generation = 0;
  std::map<std::string, Entry> entries;
  State(const Store &store, OperationOptions options) {
    throw_if_cancelled(options.stop);
    const auto refs = store.refs();
    if (const auto found = refs.find(std::string(clock_name)); found != refs.end()) {
      counter = found->second;
      require(counter->kind == Kind::chunk, "invalid asset lock counter");
      const auto data = store.get(*counter);
      const auto value = parse_json(data);
      require(tagged(value, "asset-lock-counter", 3) && value.contains("generation") &&
                  value["generation"].is_number_unsigned(),
              "invalid asset lock counter");
      generation = value["generation"].get<std::uint64_t>();
      require(generation && value.dump() == text(data), "invalid asset lock counter");
    }
    for (auto i = refs.lower_bound(std::string(prefix)); i != refs.end() && i->first.starts_with(prefix);
         ++i) {
      throw_if_cancelled(options.stop);
      require(entries.size() < max_locks, "asset lock capacity exceeded");
      const auto name = i->first.substr(prefix.size());
      validate_id(name);
      require(i->second.kind == Kind::chunk, "invalid asset lock record");
      const auto data = store.get(i->second);
      const auto value = parse_json(data);
      require(tagged(value, "asset-lock", 7) && value.contains("path") && value["path"].is_string() &&
                  value.contains("owner") && value["owner"].is_string() && value.contains("fence") &&
                  value["fence"].is_number_unsigned() && value.contains("created_at") &&
                  value["created_at"].is_number_unsigned() && value.contains("expires_at") &&
                  value["expires_at"].is_number_unsigned(),
              "invalid asset lock record");
      Entry entry{value["path"].get<std::string>(),         value["owner"].get<std::string>(),
                  value["fence"].get<std::uint64_t>(),      value["created_at"].get<std::uint64_t>(),
                  value["expires_at"].get<std::uint64_t>(), i->second};
      owner_valid(entry.owner);
      require(counter.has_value() && id(entry.path) == name && entry.fence && entry.fence <= generation &&
                  entry.created <= max_time && entry.expires > entry.created && entry.expires <= max_time &&
                  entry.json().dump() == text(data),
              "invalid asset lock record");
      entries.emplace(name, std::move(entry));
    }
  }
  std::optional<ObjectKey> key(const std::string &name) const {
    const auto found = entries.find(name);
    return found == entries.end() ? std::optional<ObjectKey>{} : std::optional{found->second.key};
  }
};
std::uint64_t expiry(std::uint64_t now, std::uint64_t seconds) {
  require(seconds >= 1 && seconds <= 86400, "asset lock duration must be in 1..86400 seconds");
  require(now <= max_time && seconds <= max_time - now, "invalid asset lock clock");
  return now + seconds;
}
std::map<std::string, std::string> requested(const std::vector<std::string> &paths) {
  require(!paths.empty() && paths.size() <= max_batch, "asset lock batch must contain 1..1024 paths");
  std::map<std::string, std::string> result;
  for (const auto &path : paths)
    require(result.emplace(id(path), path).second, "duplicate portable asset lock path");
  return result;
}
struct Prepared {
  std::map<std::string, Entry> entries;
  Json report;
};
Prepared prepare(const State &state, std::string_view owner, const std::vector<std::string> &paths,
                 std::uint64_t now, std::uint64_t seconds, OperationOptions options) {
  owner_valid(owner);
  const auto until = expiry(now, seconds);
  const auto selected = requested(paths);
  require(state.generation != UINT64_MAX, "asset lock fencing counter exhausted");
  auto count = state.entries.size();
  Json conflicts = Json::array(), locks = Json::array();
  Prepared result;
  for (const auto &[name, path] : selected) {
    report_progress(options, {ProgressPhase::planning, path, result.entries.size(), selected.size()});
    const auto found = state.entries.find(name);
    if (found != state.entries.end() && found->second.expires > now)
      conflicts.push_back(Json{{"path", path}, {"reason", "locked"}, {"lock", found->second.json()}});
    if (found == state.entries.end())
      ++count;
    Entry entry{path, std::string(owner), state.generation + 1, now, until, {Kind::chunk, {}}};
    locks.push_back(entry.json());
    result.entries.emplace(name, std::move(entry));
  }
  if (count > max_locks)
    conflicts.push_back(Json{{"path", Json{}}, {"reason", "capacity"}});
  result.report = Json{{"version", 1},
                       {"operation", "acquire"},
                       {"owner", owner},
                       {"observed_at", now},
                       {"duration_seconds", seconds},
                       {"ready", conflicts.empty()},
                       {"locks", std::move(locks)},
                       {"conflicts", std::move(conflicts)}};
  report_progress(options, {ProgressPhase::planning, {}, selected.size(), selected.size(), true});
  return result;
}
std::map<std::string, const Entry *> claims(const State &state, std::string_view owner,
                                            const std::vector<AssetLockClaim> &values, std::uint64_t now,
                                            bool active) {
  owner_valid(owner);
  require(now <= max_time, "invalid asset lock clock");
  require(!values.empty() && values.size() <= max_batch, "asset lock batch must contain 1..1024 paths");
  std::map<std::string, const Entry *> result;
  for (const auto &claim : values) {
    const auto name = id(claim.path);
    const auto found = state.entries.find(name);
    require(found != state.entries.end() && found->second.owner == owner &&
                found->second.fence == claim.fence,
            "asset lock owner or fence changed; refresh the lock");
    require(!active || found->second.expires > now, "asset lock expired; acquire it again");
    require(result.emplace(name, &found->second).second, "duplicate portable asset lock path");
  }
  return result;
}
void publish(const Store &store, const State &state, std::vector<RefUpdate> &updates,
             OperationOptions options, const std::function<void()> &check = {}) {
  report_progress(options, {ProgressPhase::publishing, "asset-locks", updates.size(), updates.size()});
  if (check)
    check();
  updates.push_back({std::string(clock_name), state.counter, state.counter});
  require(store.compare_update_refs(updates, {options.stop}), "asset lock table changed; retry operation");
}
} // namespace
AssetLocks::AssetLocks(const Store &store, std::function<std::uint64_t()> clock)
    : store_(store), clock_(clock ? std::move(clock) : std::function<std::uint64_t()>(unix_seconds)) {}
Json AssetLocks::list(const std::optional<std::string> &after, std::size_t limit,
                      OperationOptions options) const {
  require(limit >= 1 && limit <= 1000, "asset lock page limit must be in 1..1000");
  if (after)
    (void)id(*after);
  const auto cursor = after ? std::optional{portable_fold(*after)} : std::nullopt;
  const State state(store_, options);
  const auto now = clock_();
  require(now <= max_time, "invalid asset lock clock");
  std::map<std::string, const Entry *> ordered;
  for (const auto &[name, entry] : state.entries) {
    (void)name;
    ordered.emplace(portable_fold(entry.path), &entry);
  }
  Json rows = Json::array();
  auto i = cursor ? ordered.upper_bound(*cursor) : ordered.begin();
  std::optional<std::string> last;
  for (; i != ordered.end() && rows.size() < limit; ++i) {
    throw_if_cancelled(options.stop);
    auto row = i->second->json();
    row["active"] = i->second->expires > now;
    last = i->second->path;
    rows.push_back(std::move(row));
  }
  return Json{{"version", 1},
              {"observed_at", now},
              {"locks", std::move(rows)},
              {"next", i != ordered.end() && last ? Json(*last) : Json{}}};
}
Json AssetLocks::acquire_plan(std::string_view owner, const std::vector<std::string> &paths,
                              std::uint64_t seconds, OperationOptions options) const {
  const State state(store_, options);
  return prepare(state, owner, paths, clock_(), seconds, options).report;
}
Json AssetLocks::acquire(std::string_view owner, const std::vector<std::string> &paths, std::uint64_t seconds,
                         OperationOptions options) const {
  const State state(store_, options);
  auto prepared = prepare(state, owner, paths, clock_(), seconds, options);
  require(prepared.report["ready"].get<bool>(),
          "asset lock acquisition is blocked; inspect locks acquire-plan");
  std::vector<RefUpdate> updates;
  for (auto &[name, entry] : prepared.entries) {
    throw_if_cancelled(options.stop);
    entry.key = store_.put(Kind::chunk, bytes(entry.json().dump())).first;
    updates.push_back({std::string(prefix) + name, state.key(name), entry.key});
  }
  const auto value =
      Json{{"version", 1}, {"type", "asset-lock-counter"}, {"generation", state.generation + 1}};
  const auto next = store_.put(Kind::chunk, bytes(value.dump())).first;
  prepared.report["state"] = "acquired";
  report_progress(options, {ProgressPhase::publishing, "asset-locks", updates.size(), updates.size()});
  const auto published_at = clock_();
  require(published_at >= prepared.report["observed_at"].get<std::uint64_t>(),
          "asset lock clock moved backwards; retry operation");
  require(published_at < prepared.entries.begin()->second.expires, "asset lock expired before publication");
  updates.push_back({std::string(clock_name), state.counter, next});
  require(store_.compare_update_refs(updates, {options.stop}), "asset lock table changed; retry operation");
  return std::move(prepared.report);
}
Json AssetLocks::renew(std::string_view owner, const std::vector<AssetLockClaim> &values,
                       std::uint64_t seconds, OperationOptions options) const {
  const State state(store_, options);
  const auto now = clock_(), until = expiry(now, seconds);
  const auto selected = claims(state, owner, values, now, true);
  Json rows = Json::array();
  std::vector<RefUpdate> updates;
  auto deadline = max_time;
  for (const auto &[name, original] : selected) {
    throw_if_cancelled(options.stop);
    auto entry = *original;
    entry.expires = std::max(until, entry.expires);
    deadline = std::min(deadline, original->expires);
    const auto next = store_.put(Kind::chunk, bytes(entry.json().dump())).first;
    rows.push_back(entry.json());
    updates.push_back({std::string(prefix) + name, original->key, next});
  }
  auto report =
      Json{{"version", 1}, {"operation", "renew"}, {"state", "renewed"}, {"locks", std::move(rows)}};
  publish(store_, state, updates, options, [&] {
    const auto published_at = clock_();
    require(published_at >= now, "asset lock clock moved backwards; retry operation");
    require(published_at < deadline, "asset lock expired before publication");
  });
  return report;
}
Json AssetLocks::release(std::string_view owner, const std::vector<AssetLockClaim> &values,
                         OperationOptions options) const {
  const State state(store_, options);
  const auto selected = claims(state, owner, values, clock_(), false);
  std::vector<RefUpdate> updates;
  Json rows = Json::array();
  for (const auto &[name, entry] : selected) {
    rows.push_back(entry->json());
    updates.push_back({std::string(prefix) + name, entry->key, {}});
  }
  auto report =
      Json{{"version", 1}, {"operation", "release"}, {"state", "released"}, {"locks", std::move(rows)}};
  publish(store_, state, updates, options);
  return report;
}
std::uint64_t AssetLocks::validate(std::string_view owner, const std::vector<AssetLockClaim> &values,
                                   OperationOptions options) const {
  const State state(store_, options);
  const auto selected = claims(state, owner, values, clock_(), true);
  auto deadline = max_time;
  for (const auto &[name, entry] : selected) {
    (void)name;
    deadline = std::min(deadline, entry->expires);
  }
  return deadline;
}
Json AssetLocks::prune(bool remove, OperationOptions options) const {
  const State state(store_, options);
  const auto now = clock_();
  require(now <= max_time, "invalid asset lock clock");
  std::vector<RefUpdate> updates;
  Json rows = Json::array();
  for (const auto &[name, entry] : state.entries) {
    throw_if_cancelled(options.stop);
    if (entry.expires <= now) {
      rows.push_back(entry.json());
      updates.push_back({std::string(prefix) + name, entry.key, {}});
    }
  }
  auto report = Json{{"version", 1}, {"operation", "prune"}, {"applied", remove}, {"locks", std::move(rows)}};
  if (remove && !updates.empty())
    publish(store_, state, updates, options,
            [&] { require(clock_() >= now, "asset lock clock moved backwards; retry operation"); });
  return report;
}
} // namespace syrnck
