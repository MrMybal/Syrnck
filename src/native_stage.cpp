#include "native_workspace.hpp"
#include <chrono>
namespace syrnck {
namespace {
using Files = std::map<std::string, Json>;
Files files_from(const Json &revision) {
  Files files;
  for (const auto &[path, entry] : revision["files"].items())
    files.emplace_hint(files.end(), path, entry);
  return files;
}
Json files_json(const Files &files) {
  Json result = Json::object();
  auto &object = result.get_ref<Json::object_t &>();
  object.reserve(files.size());
  for (const auto &[path, entry] : files)
    object.emplace_back(path, entry);
  return result;
}
std::optional<ObjectKey> lookup(const Refs &refs, const std::string &name) {
  const auto found = refs.find(name);
  return found == refs.end() ? std::optional<ObjectKey>{} : std::optional{found->second};
}
std::optional<std::string> id(const std::optional<ObjectKey> &key) {
  return key ? std::optional{key->id} : std::optional<std::string>{};
}
Json nullable_id(const std::optional<ObjectKey> &key) {
  return key ? Json(key->id) : Json{};
}
struct Stage {
  std::string head_name, stage_name, marker;
  std::optional<ObjectKey> head, index, base;
  Files files, baseline;
  Stage(const Store &store, std::string_view branch) {
    validate_ref(branch);
    head_name = "heads/" + std::string(branch);
    stage_name = "staging/" + std::string(branch);
    validate_ref(head_name);
    validate_ref(stage_name);
    marker = "Syrnck staging v1: " + std::string(branch);
    const auto refs = store.refs();
    head = lookup(refs, head_name);
    require(!head || head->kind == Kind::revision, "branch must point to a revision");
    index = lookup(refs, stage_name);
    base = head;
    if (index) {
      require(index->kind == Kind::revision, "invalid native staging snapshot");
      const auto revision = store.metadata(Kind::revision, index->id);
      require(revision["message"] == marker && revision["timestamp"] == 0 && revision["parents"].size() <= 1,
              "invalid native staging snapshot");
      base = revision["parents"].empty()
                 ? std::optional<ObjectKey>{}
                 : std::optional{ObjectKey{Kind::revision, revision["parents"][0].get<std::string>()}};
      files = files_from(revision);
    }
    if (base)
      baseline = files_from(store.metadata(Kind::revision, base->id));
    if (!index)
      files = baseline;
  }
  void require_fresh() const {
    require(head == base, "native staging baseline changed; inspect or reset the staging index");
  }
  ObjectKey snapshot(const Store &store, const Files &entries) const {
    const auto revision = Json{{"version", 1},
                               {"parents", base ? Json::array({base->id}) : Json::array()},
                               {"message", marker},
                               {"timestamp", 0},
                               {"files", files_json(entries)}};
    return store.put(Kind::revision, bytes(revision.dump())).first;
  }
  void publish(const Store &store, const ObjectKey &next, OperationOptions options) const {
    require(store.compare_update_refs({{head_name, head, head}, {stage_name, index, next}}, options),
            "native staging references changed; retry operation");
  }
};
class Selection {
  std::map<std::string, bool> paths_;
  bool all_ = false;

public:
  explicit Selection(const std::vector<std::string> &paths) {
    require(!paths.empty(), "native staging requires at least one path");
    for (const auto &path : paths) {
      if (path == ".")
        all_ = true;
      else {
        validate_path(path);
        paths_.emplace(path, false);
      }
    }
  }
  bool matches(std::string_view path) {
    bool matched = all_;
    for (std::size_t p = 0;;) {
      const auto n = path.find('/', p);
      if (auto found = paths_.find(std::string(path.substr(0, n))); found != paths_.end()) {
        found->second = true;
        matched = true;
      }
      if (n == path.npos)
        break;
      p = n + 1;
    }
    return matched;
  }
  void observe(const Files &files, std::stop_token stop) {
    for (const auto &[path, entry] : files) {
      (void)entry;
      throw_if_cancelled(stop);
      matches(path);
    }
  }
  void validate() const {
    for (const auto &[path, matched] : paths_) {
      (void)path;
      require(matched, "native path selection did not match any file");
    }
  }
};
} // namespace
std::string Store::stage_directory(const fs::path &directory, std::string_view branch,
                                   const std::vector<std::string> &paths) const {
  return stage_directory(directory, branch, paths, settings().ingest_options());
}
std::string Store::stage_directory(const fs::path &directory, std::string_view branch,
                                   const std::vector<std::string> &paths, IngestOptions options) const {
  throw_if_cancelled(options.stop);
  Stage stage(*this, branch);
  require_no_native_checkout(*this);
  stage.require_fresh();
  Selection selection(paths);
  selection.observe(stage.baseline, options.stop);
  auto files = stage.files;
  for (auto it = files.begin(); it != files.end();) {
    throw_if_cancelled(options.stop);
    if (selection.matches(it->first))
      it = files.erase(it);
    else
      ++it;
  }
  visit_native_files(directory, root(),
                     [&](const NativeFile &file) {
                       if (selection.matches(file.relative))
                         files.emplace(
                             file.relative,
                             ingest_native_file(*this, file, options,
                                                "file changed during native staging; retry operation"));
                     },
                     {options.stop, options.progress});
  selection.validate();
  throw_if_cancelled(options.stop);
  const auto next = stage.snapshot(*this, files);
  stage.publish(*this, next, {options.stop, options.progress});
  return next.id;
}
Json Store::staged_status(std::string_view branch, OperationOptions options) const {
  throw_if_cancelled(options.stop);
  const Stage stage(*this, branch);
  auto difference =
      compare_native_snapshots(*this, id(stage.base), id(stage.index ? stage.index : stage.head), options);
  return Json{{"version", 1},
              {"branch", branch},
              {"base", nullable_id(stage.base)},
              {"head", nullable_id(stage.head)},
              {"snapshot", nullable_id(stage.index)},
              {"stale", stage.head != stage.base},
              {"clean", difference["equal"]},
              {"staged_files", difference["target_files"]},
              {"counts", std::move(difference["counts"])},
              {"changes", std::move(difference["changes"])}};
}
std::optional<std::string> Store::reset_stage(std::string_view branch, const std::vector<std::string> &paths,
                                              OperationOptions options) const {
  throw_if_cancelled(options.stop);
  require_no_native_checkout(*this);
  const Stage stage(*this, branch);
  if (paths.empty()) {
    if (stage.index)
      require(compare_update_refs({{stage.stage_name, stage.index, {}}}, options),
              "native staging references changed; retry operation");
    return {};
  }
  require(stage.index.has_value(), "no native staging snapshot");
  stage.require_fresh();
  Selection selection(paths);
  auto files = stage.files;
  for (auto it = files.begin(); it != files.end();) {
    throw_if_cancelled(options.stop);
    if (selection.matches(it->first))
      it = files.erase(it);
    else
      ++it;
  }
  for (const auto &[path, entry] : stage.baseline) {
    throw_if_cancelled(options.stop);
    if (selection.matches(path))
      files.insert_or_assign(path, entry);
  }
  selection.validate();
  throw_if_cancelled(options.stop);
  const auto next = stage.snapshot(*this, files);
  stage.publish(*this, next, options);
  return next.id;
}
Json Store::workspace_status(const fs::path &directory, std::string_view branch,
                             OperationOptions options) const {
  auto staged = staged_status(branch, options);
  const auto &effective = staged["snapshot"].is_null() ? staged["head"] : staged["snapshot"];
  const auto index =
      effective.is_null() ? std::optional<std::string>{} : std::optional{effective.get<std::string>()};
  auto unstaged = status_directory(directory, index, options);
  const auto clean =
      !staged["stale"].get<bool>() && staged["clean"].get<bool>() && unstaged["clean"].get<bool>();
  return Json{{"version", 1},
              {"branch", branch},
              {"clean", clean},
              {"staged", std::move(staged)},
              {"unstaged", std::move(unstaged)}};
}
std::string Store::commit_staged(std::string_view branch, std::string_view message,
                                 OperationOptions options) const {
  throw_if_cancelled(options.stop);
  require_no_native_checkout(*this);
  const Stage stage(*this, branch);
  require(stage.index.has_value(), "no native staging snapshot");
  stage.require_fresh();
  require(stage.files != stage.baseline, "no staged native changes");
  const auto timestamp = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
          .count());
  const auto revision = Json{{"version", 1},
                             {"parents", stage.base ? Json::array({stage.base->id}) : Json::array()},
                             {"message", message},
                             {"timestamp", timestamp},
                             {"files", files_json(stage.files)}};
  throw_if_cancelled(options.stop);
  const auto next = put(Kind::revision, bytes(revision.dump())).first;
  require(compare_update_refs({{stage.head_name, stage.head, next}, {stage.stage_name, stage.index, {}}},
                              options),
          "native staging references changed; retry operation");
  return next.id;
}
} // namespace syrnck
