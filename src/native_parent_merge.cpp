#include "native_parent_merge.hpp"
namespace syrnck {
namespace {
const NativeSnapshotFile *lookup(const NativeSnapshot &files, const std::string &path) {
  const auto at = files.find(path);
  return at == files.end() ? nullptr : &at->second;
}
Json logical(const NativeSnapshotFile *file) {
  return file ? Json{{"size", file->content.size},
                     {"hash", file->content.hash},
                     {"executable", file->executable}}
              : Json{};
}
Json logical_json(const Json &file) {
  return file.is_null()
             ? Json{}
             : Json{{"size", file["size"]}, {"hash", file["file_hash"]}, {"executable", file["executable"]}};
}
std::string identity(const std::vector<std::string> &parents, const NativeSnapshot &snapshot,
                     const std::function<void(std::size_t)> &work, OperationOptions options) {
  Json files = Json::object();
  auto &entries = files.get_ref<Json::object_t &>();
  entries.reserve(snapshot.size());
  for (const auto &[path, file] : snapshot) {
    throw_if_cancelled(options.stop);
    if (work)
      work(1);
    entries.emplace_back(path, Json{{"manifest", file.manifest}, {"executable", file.executable}});
  }
  const auto encoded = Json{
      {"version", 1},
      {"parents", parents},
      {"message", "Syrnck virtual parent merge v1"},
      {"timestamp", 0},
      {"files",
       std::move(files)}}.dump();
  require(encoded.size() <= max_object_size, "native virtual parent merge exceeds 16 MiB");
  return object_id(Kind::revision, bytes(encoded));
}
} // namespace
NativeParentMerge merge_native_parents(const Store &store, const NativeAncestryGraph &graph,
                                       const std::vector<std::string> &parents,
                                       const NativeMergeBaseLimits &limits, const NativeSnapshotReader &read,
                                       NativeTextMergeBatch *text, NativeRenameBatch *renames,
                                       const std::function<void(std::size_t)> &work,
                                       OperationOptions options) {
  throw_if_cancelled(options.stop);
  NativeParentMerge result;
  if (!parents.empty())
    result.files = read(parents.front(), options);
  std::vector<std::string> group;
  if (!parents.empty())
    group.push_back(parents.front());
  Json stages = Json::array();
  std::size_t report_bytes = 4096;
  for (std::size_t i = 1; i < parents.size(); ++i) {
    throw_if_cancelled(options.stop);
    const auto bases = graph.compare(group, {parents[i]}, options, work).bases;
    NativeMergeBase baseline;
    if (bases.size() == 1) {
      baseline.id = bases.front();
      baseline.files = read(bases.front(), options);
    } else if (bases.size() > 1)
      baseline =
          synthesize_native_merge_base(store, graph, bases, limits, options, text, renames, {read, work});
    auto incoming = read(parents[i], options);
    auto uncertain = baseline.ambiguous;
    uncertain.insert(result.ambiguous.begin(), result.ambiguous.end());
    if (work) {
      work(baseline.files.size());
      work(result.files.size());
      work(incoming.size());
    }
    auto merged = merge_native_files(store, baseline.files, result.files, incoming, options, nullptr, nullptr,
                                     text, nullptr, renames, false, &uncertain);
    std::map<std::string, const Json *> conflicts;
    for (const auto &row : merged.conflicts) {
      const auto path = row["path"].get<std::string>();
      result.ambiguous.insert(path);
      conflicts.emplace(path, &row);
      if (row.contains("other")) {
        const auto other = row["other"].get<std::string>();
        result.ambiguous.insert(other);
        result.reusable.erase(other);
      }
    }
    for (const auto &path : result.ambiguous) {
      throw_if_cancelled(options.stop);
      if (work)
        work(1);
      Json triple{{"base", logical(lookup(baseline.files, path))},
                  {"current", logical(lookup(result.files, path))},
                  {"incoming", logical(lookup(incoming, path))},
                  {"base_ambiguous", baseline.ambiguous.contains(path)}};
      bool reusable = result.provenance.contains(path) ? result.reusable.contains(path) : true;
      if (const auto found = conflicts.find(path); found != conflicts.end()) {
        const auto &row = *found->second;
        const auto reason = row["reason"].get<std::string>();
        triple["reason"] = reason;
        const bool ordinary = reason == "content" || reason == "add_add" || reason == "modify_delete" ||
                              reason == "delete_modify";
        reusable = reusable && ordinary;
        if (ordinary) {
          triple["base"] = logical_json(row["base"]);
          triple["current"] = logical_json(row["current"]);
          triple["incoming"] = logical_json(row["incoming"]);
        }
        if (row.contains("other"))
          triple["other"] = row["other"];
      } else if (!result.provenance.contains(path))
        reusable = false;
      reusable = reusable && !baseline.ambiguous.contains(path);
      const auto previous = result.provenance.find(path);
      auto trace = std::string("Syrnck native merge provenance v1:");
      if (previous != result.provenance.end())
        trace += previous->second;
      trace += triple.dump();
      result.provenance[path] = hash(bytes(trace));
      if (reusable)
        result.reusable.insert(path);
      else
        result.reusable.erase(path);
    }
    Json row{{"parents", group},
             {"incoming", parents[i]},
             {"merge_bases", bases},
             {"base_synthesis", std::move(baseline.report)},
             {"conflicts", std::move(merged.conflicts)},
             {"text_merge", std::move(merged.text_merge)},
             {"renames", std::move(merged.renames)}};
    const auto cost = row.dump().size() + 1;
    require(cost <= max_object_size - report_bytes, "native parent merge report exceeds 16 MiB");
    report_bytes += cost;
    stages.push_back(std::move(row));
    result.files = std::move(merged.files);
    group.push_back(parents[i]);
  }
  result.id = parents.size() == 1 ? parents.front() : identity(parents, result.files, work, options);
  result.report = Json{{"id", result.id},
                       {"parents", parents},
                       {"ambiguous_paths", result.ambiguous},
                       {"stages", std::move(stages)}};
  require(result.report.dump().size() <= max_object_size, "native parent merge report exceeds 16 MiB");
  return result;
}
} // namespace syrnck
