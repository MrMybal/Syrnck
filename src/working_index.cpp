#include <syrnck/working_index.hpp>
#include "working_file_stamp.hpp"
#include "native_context.hpp"
namespace syrnck {
namespace {
fs::path pointer_path(const Store &store) { return native_workspace_local_path(store, "working-index.json"); }
std::optional<Bytes> pointer_bytes(const Store &store) {
  const auto path = pointer_path(store);
  const auto status = fs::symlink_status(path);
  if (!fs::exists(status))
    return {};
  require(fs::is_regular_file(status) && !native_link_like(path, status), "invalid working index pointer");
  return read_bytes(path, 4096);
}
struct Entry {
  NativeContent content;
  bool executable;
  std::string stamp;
};
struct Cache {
  std::string state = "missing";
  std::string generation = hash(random_bytes(16));
  std::optional<Bytes> pointer;
  std::map<std::string, Entry> entries;
};
Cache load_cache(const Store &store, std::string_view workspace, OperationOptions options) {
  Cache cache;
  cache.pointer = pointer_bytes(store);
  if (!cache.pointer)
    return cache;
  cache.state = "invalid";
  try {
    const auto pointer = parse_json(*cache.pointer);
    require(pointer.is_object() && pointer.size() == 3 && pointer.at("version") == 1,
            "invalid working index pointer");
    const auto id = pointer.at("object").get<std::string>();
    validate_id(id);
    if (pointer.at("workspace").get<std::string>() != workspace) {
      cache.state = "other_workspace";
      return cache;
    }
    const auto value = parse_json(store.get({Kind::chunk, id}));
    require(value.is_object() && value.size() == 5 && value.at("version") == 1 &&
                value.at("type") == "native-working-index" && value.at("workspace").get<std::string>() == workspace &&
                value.at("entries").is_array(), "invalid working index contents");
    const auto generation = value.at("generation").get<std::string>();
    validate_id(generation);
    std::string previous;
    for (const auto &row : value["entries"]) {
      throw_if_cancelled(options.stop);
      require(row.is_array() && row.size() == 5 && row[1].is_number_unsigned() && row[3].is_boolean(),
              "invalid working index contents");
      auto path = row[0].get<std::string>();
      validate_path(path);
      require(previous.empty() || previous < path, "invalid working index contents");
      previous = path;
      auto digest = row[2].get<std::string>(), stamp = row[4].get<std::string>();
      validate_id(digest);
      validate_id(stamp);
      cache.entries.emplace(std::move(path), Entry{{row[1].get<std::uint64_t>(), std::move(digest)},
                                                   row[3].get<bool>(), std::move(stamp)});
    }
    cache.generation = generation;
    cache.state = "ready";
  } catch (const Cancelled &) {
    throw;
  } catch (const Error &) {
    cache.entries.clear();
  } catch (const Json::exception &) {
    cache.entries.clear();
  }
  return cache;
}
} // namespace
Json indexed_native_observation(const Store &store, const fs::path &directory,
                                const std::optional<std::string> &revision, NativeSnapshot &observed,
                                OperationOptions options) {
  throw_if_cancelled(options.stop);
  const auto materialization = read_native_materialization(store).policy;
  auto identity = utf8(fs::canonical(directory));
  if (!materialization.complete()) {
    identity.push_back('\0');
    identity += materialization.identity();
  }
  const auto workspace = hash(bytes(identity));
  auto cache = load_cache(store, workspace, options);
  const auto baseline = revision ? read_native_materialized_snapshot(store, *revision, materialization, options) : NativeSnapshot{};
  NativeSnapshot current;
  std::map<std::string, Entry> next;
  Bytes buffer(1024 * 1024);
  std::uint64_t hashed_bytes = 0, hashed_files = 0, reused_files = 0, supported_files = 0;
  bool truncated = false;
  std::size_t budget = 512;
  visit_native_files(directory, store, [&](const NativeFile &file) {
    const auto before = working_file_stamp(file);
    const auto found = cache.entries.find(file.relative);
    NativeContent content;
    if (before && before->reusable && found != cache.entries.end() && found->second.stamp == before->id &&
        found->second.content.size == file.size && found->second.executable == file.executable) {
      content = found->second.content;
      ++reused_files;
      report_progress(options, {ProgressPhase::planning, file.relative, reused_files});
    } else {
      content = hash_native_file(file, buffer, options);
      require(content.size <= UINT64_MAX - hashed_bytes, "native comparison byte count overflow");
      hashed_bytes += content.size;
      ++hashed_files;
    }
    const auto after = working_file_stamp(file);
    // Enforce the same stable identity around hashing or cached observation.
    // A failed metadata query cannot validate an earlier supported observation.
    if (before)
      require(after && before->id == after->id, "file changed during indexed native scan; retry operation");
    if (before && after) {
      ++supported_files;
      const auto row = Json::array({file.relative, content.size, content.hash, file.executable, after->id});
      const auto size = row.dump().size() + 1;
      if (size <= max_object_size - budget) {
        budget += size;
        next.emplace(file.relative, Entry{content, file.executable, after->id});
      } else
        truncated = true;
    }
    current.emplace(file.relative, NativeSnapshotFile{{}, std::move(content), file.executable});
  }, options);
  auto comparison = compare_native_workspace_maps(baseline, current, options);
  Json rows = Json::array();
  for (const auto &[path, entry] : next)
    rows.push_back(Json::array({path, entry.content.size, entry.content.hash, entry.executable, entry.stamp}));
  const auto document = Json{{"version", 1}, {"type", "native-working-index"}, {"workspace", workspace},
                             {"generation", cache.generation},
                             {"entries", std::move(rows)}}.dump();
  require(document.size() <= max_object_size, "working index exceeds 16 MiB");
  // Observers/cancellation precede the append and pointer replacement. Failure
  // after the append can leave a collectible chunk, never a partial pointer.
  report_progress(options, {ProgressPhase::publishing, "working-index", 0, 1});
  require(pointer_bytes(store) == cache.pointer, "working index changed; retry operation");
  const auto key = store.put(Kind::chunk, bytes(document)).first;
  throw_if_cancelled(options.stop);
  const auto pointer = Json{{"version", 1}, {"workspace", workspace}, {"object", key.id}}.dump();
  if (!cache.pointer || text(*cache.pointer) != pointer)
    atomic_write(pointer_path(store), bytes(pointer), true, options.stop);
  const auto workspace_files = current.size();
  observed = std::move(current);
  return Json{{"version", 1}, {"base", revision ? Json(*revision) : Json{}},
              {"clean", comparison["changes"].empty()}, {"tracked_files", baseline.size()},
              {"workspace_files", workspace_files}, {"hashed_bytes", hashed_bytes},
              {"counts", std::move(comparison["counts"])}, {"changes", std::move(comparison["changes"])},
              {"working_index", Json{{"version", 1}, {"mode", "stat-v1"}, {"previous", cache.state},
                                      {"hashed_files", hashed_files}, {"reused_files", reused_files},
                                      {"supported_files", supported_files}, {"cached_files", next.size()},
                                      {"truncated", truncated}, {"object", key.id}}}};
}
Json indexed_native_status(const Store &store, const fs::path &directory,
                           const std::optional<std::string> &revision, OperationOptions options) {
  NativeSnapshot observed;
  return indexed_native_observation(store, directory, revision, observed, options);
}
Json indexed_native_workspace_status(const Store &store, const fs::path &directory, std::string_view branch,
                                     OperationOptions options) {
  auto staged = store.staged_status(branch, options);
  const auto &effective = staged["snapshot"].is_null() ? staged["head"] : staged["snapshot"];
  const auto revision = effective.is_null() ? std::optional<std::string>{}
                                           : std::optional{effective.get<std::string>()};
  auto unstaged = indexed_native_status(store, directory, revision, options);
  const auto clean = !staged["stale"].get<bool>() && staged["clean"].get<bool>() && unstaged["clean"].get<bool>();
  return Json{{"version", 1}, {"branch", branch}, {"clean", clean},
              {"staged", std::move(staged)}, {"unstaged", std::move(unstaged)}};
}
Json clear_native_working_index(const Store &store, OperationOptions options) {
  throw_if_cancelled(options.stop);
  const auto original = pointer_bytes(store);
  report_progress(options, {ProgressPhase::publishing, "working-index", 0, 1});
  require(pointer_bytes(store) == original, "working index changed; retry operation");
  if (original) {
    require(fs::remove(pointer_path(store)), "cannot clear working index");
    sync_directory(pointer_path(store).parent_path());
  }
  return Json{{"version", 1}, {"state", original ? "cleared" : "absent"}};
}
} // namespace syrnck
