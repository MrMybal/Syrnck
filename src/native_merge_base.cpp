#include "native_merge_base.hpp"
#include <algorithm>
namespace syrnck {
void NativeMergeBaseLimits::validate() const {
  require(max_bases >= 2 && max_bases <= 1024, "native merge base count limit must be in 2..1024");
  require(max_virtual_merges >= 1 && max_virtual_merges <= 4096,
          "native virtual merge limit must be in 1..4096");
  require(max_depth >= 1 && max_depth <= 128, "native merge base depth limit must be in 1..128");
  require(max_work >= 1 && max_work <= 256 * 1024 * 1024,
          "native merge base work limit must be in 1..268435456");
  require(max_input_bytes >= 1024 && max_input_bytes <= 256 * 1024 * 1024,
          "native merge base input limit must be in 1024..268435456");
}
namespace {
class Synthesis {
  const Store &store_;
  const NativeAncestryGraph &graph_;
  const NativeMergeBaseLimits &limits_;
  OperationOptions options_;
  NativeTextMergeBatch *text_;
  NativeRenameBatch *renames_;
  NativeMergeBaseCallbacks callbacks_;
  std::size_t work_ = 0, input_ = 0, merges_ = 0, depth_ = 0, report_bytes_ = 4096;
  Json steps_ = Json::array();
  void work(std::size_t amount) {
    throw_if_cancelled(options_.stop);
    require(amount <= limits_.max_work - work_, "native merge base work limit exceeded");
    work_ += amount;
    if (callbacks_.work) callbacks_.work(amount);
  }
  void input(std::size_t amount) {
    require(amount <= limits_.max_input_bytes - input_, "native merge base input limit exceeded");
    input_ += amount;
  }
  NativeMergeBase load(const std::string &id) {
    work(1);
    if (callbacks_.read) {
      auto files = callbacks_.read(id, options_);
      input(256);
      for (const auto &[path, file] : files) {
        work(1); input(Json(path).dump().size() + file.json().dump().size() + 3);
      }
      return NativeMergeBase{id, std::move(files), {}, {}};
    }
    const auto revision = store_.metadata(Kind::revision, id);
    input(revision.dump().size());
    NativeMergeBase result{id, {}, {}, {}};
    // A small per-snapshot manifest cache avoids decoding identical assets for
    // every path. The input budget charges each decoded canonical object.
    std::map<std::string, NativeContent> manifests;
    for (const auto &[path, entry] : revision["files"].items()) {
      work(1);
      const auto manifest = entry["manifest"].get<std::string>();
      auto found = manifests.find(manifest);
      if (found == manifests.end()) {
        const auto value = store_.metadata(Kind::manifest, manifest);
        input(value.dump().size());
        if (manifests.size() == 1024)
          manifests.clear();
        found = manifests
                    .emplace(manifest, NativeContent{value["size"].get<std::uint64_t>(),
                                                     value["file_hash"].get<std::string>()})
                    .first;
      }
      result.files.emplace_hint(result.files.end(), path,
                                NativeSnapshotFile{manifest, found->second, entry["executable"].get<bool>()});
    }
    return result;
  }
  NativeMergeBase build(std::vector<std::string> bases, std::size_t depth) {
    work(1);
    require(bases.size() <= limits_.max_bases, "native merge base count limit exceeded");
    std::sort(bases.begin(), bases.end());
    bases.erase(std::unique(bases.begin(), bases.end()), bases.end());
    if (bases.size() > 1) {
      require(depth <= limits_.max_depth, "native merge base depth limit exceeded");
      depth_ = std::max(depth_, depth);
    }
    if (bases.empty())
      return {};
    auto result = load(bases.front());
    std::vector<std::string> parents{bases.front()};
    for (std::size_t i = 1; i < bases.size(); ++i) {
      work(1);
      require(merges_ < limits_.max_virtual_merges, "native virtual merge limit exceeded");
      ++merges_;
      const auto ancestry = graph_.compare(parents, {bases[i]}, options_, [&](auto cost) { work(cost); });
      auto ancestor = build(ancestry.bases, depth + 1);
      auto incoming = load(bases[i]);
      auto ambiguous = std::move(ancestor.ambiguous);
      ambiguous.insert(result.ambiguous.begin(), result.ambiguous.end());
      work(ancestor.files.size());
      work(result.files.size());
      work(incoming.files.size());
      auto merged =
          merge_native_files(store_, ancestor.files, result.files, incoming.files, options_, nullptr, nullptr,
                             text_, nullptr, ambiguous.empty() ? renames_ : nullptr, false, &ambiguous);
      for (const auto &conflict : merged.conflicts) {
        ambiguous.insert(conflict["path"].get<std::string>());
        if (conflict.contains("other"))
          ambiguous.insert(conflict["other"].get<std::string>());
      }
      Json files = Json::object();
      auto &entries = files.get_ref<Json::object_t &>();
      entries.reserve(merged.files.size());
      for (const auto &[path, file] : merged.files) {
        work(1);
        entries.emplace_back(path, Json{{"manifest", file.manifest}, {"executable", file.executable}});
      }
      const auto encoded = Json{
          {"version", 1},
          {"parents", Json::array({*result.id, bases[i]})},
          {"message", "Syrnck virtual merge base v1"},
          {"timestamp", 0},
          {"files",
           std::move(files)}}.dump();
      input(encoded.size());
      require(encoded.size() <= max_object_size, "native virtual merge base exceeds 16 MiB");
      auto id = object_id(Kind::revision, bytes(encoded));
      Json row{{"id", id},
               {"parents", Json::array({*result.id, bases[i]})},
               {"merge_bases", ancestry.bases},
               {"depth", depth},
               {"ambiguous_paths", ambiguous},
               {"text_merge", std::move(merged.text_merge)},
               {"renames", std::move(merged.renames)}};
      const auto cost = row.dump().size() + 1;
      require(cost <= max_object_size - report_bytes_, "native merge base report exceeds 16 MiB");
      report_bytes_ += cost;
      steps_.push_back(std::move(row));
      result = NativeMergeBase{std::move(id), std::move(merged.files), std::move(ambiguous), {}};
      parents.push_back(bases[i]);
    }
    return result;
  }

public:
  Synthesis(const Store &store, const NativeAncestryGraph &graph, const NativeMergeBaseLimits &limits,
            OperationOptions options, NativeTextMergeBatch *text, NativeRenameBatch *renames,
            NativeMergeBaseCallbacks callbacks)
      : store_(store), graph_(graph), limits_(limits), options_(std::move(options)), text_(text),
        renames_(renames), callbacks_(std::move(callbacks)) {}
  NativeMergeBase run(std::vector<std::string> bases) {
    auto result = build(bases, 1);
    result.report = Json{{"version", 1},
                         {"algorithm", "recursive_v1"},
                         {"merge_bases", bases},
                         {"virtual", true},
                         {"id", result.id ? Json(*result.id) : Json{}},
                         {"ambiguous_paths", result.ambiguous},
                         {"virtual_merges", merges_},
                         {"depth", depth_},
                         {"work", work_},
                         {"input_bytes", input_},
                         {"limits", Json{{"max_bases", limits_.max_bases},
                                         {"max_virtual_merges", limits_.max_virtual_merges},
                                         {"max_depth", limits_.max_depth},
                                         {"max_work", limits_.max_work},
                                         {"max_input_bytes", limits_.max_input_bytes}}},
                         {"steps", std::move(steps_)}};
    return result;
  }
};
} // namespace
NativeMergeBase synthesize_native_merge_base(const Store &store, const NativeAncestryGraph &graph,
                                             std::vector<std::string> bases,
                                             const NativeMergeBaseLimits &limits, OperationOptions options,
                                             NativeTextMergeBatch *text, NativeRenameBatch *renames,
                                             NativeMergeBaseCallbacks callbacks) {
  throw_if_cancelled(options.stop);
  limits.validate();
  require(bases.size() > 1, "native virtual merge requires multiple bases");
  return Synthesis(store, graph, limits, std::move(options), text, renames, std::move(callbacks)).run(std::move(bases));
}
} // namespace syrnck
