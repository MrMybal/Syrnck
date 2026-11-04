#include "git_internal.hpp"
#include <syrnck/git.hpp>
#include <algorithm>

namespace syrnck::git {
namespace detail {
void validate_oid(std::string_view oid) {
  require((oid.size() == 40 || oid.size() == 64) &&
              std::all_of(oid.begin(), oid.end(),
                          [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }),
          "invalid Git object ID");
}
std::string trim_line(std::string value) {
  while (!value.empty() && (value.back() == '\n' || value.back() == '\r'))
    value.pop_back();
  return value;
}
ProcessResult local_git(const fs::path &directory, std::vector<std::string> arguments, std::string_view input,
                        std::size_t limit, std::stop_token stop) {
  throw_if_cancelled(stop);
  std::vector<std::string> command{"git", "--no-pager",          "--no-replace-objects", "--no-lazy-fetch",
                                   "-c",  "core.fsmonitor=false"};
  command.insert(command.end(), arguments.begin(), arguments.end());
  auto result = process(command, directory, input, limit);
  throw_if_cancelled(stop);
  return result;
}
std::string resolve_commit(const fs::path &directory, std::string_view revision, std::stop_token stop) {
  require(!revision.empty() && revision.size() <= 4096 &&
              revision.find_first_of("\r\n\0", 0, 3) == revision.npos,
          "invalid Git revision argument");
  auto result =
      local_git(directory, {"rev-parse", "--verify", "--end-of-options", std::string(revision) + "^{commit}"},
                {}, 1024, stop);
  require(result.status == 0, "cannot resolve Git commit");
  auto oid = trim_line(std::move(result.out));
  validate_oid(oid);
  return oid;
}
} // namespace detail
Json Catalog::json() const {
  Json entries = Json::array();
  for (const auto &asset : assets)
    entries.push_back(Json{{"path", asset.path},
                           {"blob", asset.blob},
                           {"manifest", asset.manifest},
                           {"size", asset.size},
                           {"executable", asset.executable}});
  return Json{{"version", 1}, {"commit", commit}, {"assets", entries}};
}
Catalog catalog(const fs::path &directory, std::string_view revision, OperationOptions operation) {
  Catalog result{detail::resolve_commit(directory, revision, operation.stop), {}};
  auto entries = detail::read_tree(directory, result.commit, operation);
  std::map<std::string, Pointer> pointers;
  detail::visit_small_blobs(
      directory, entries, result.commit,
      [&](const std::string &oid, std::string_view value) {
        throw_if_cancelled(operation.stop);
        if (auto pointer = parse_pointer(value))
          pointers.emplace(oid, *pointer);
      },
      operation);
  for (const auto &entry : entries)
    if (auto pointer = pointers.find(entry.oid); entry.regular() && pointer != pointers.end())
      result.assets.push_back(
          {entry.path, entry.oid, pointer->second.manifest, pointer->second.size, entry.mode == "100755"});
  std::sort(result.assets.begin(), result.assets.end(),
            [](const auto &a, const auto &b) { return a.path < b.path; });
  return result;
}
Bytes bundle_bytes(const Catalog &catalog) {
  detail::validate_oid(catalog.commit);
  std::map<std::string, Json> sorted;
  for (const auto &asset : catalog.assets) {
    validate_path(asset.path);
    validate_id(asset.manifest);
    require(sorted.emplace(asset.path, Json{{"manifest", asset.manifest}, {"executable", asset.executable}})
                .second,
            "duplicate Git asset path");
  }
  Json files = Json::object();
  for (auto &[path, entry] : sorted)
    files[path] = std::move(entry);
  auto value = Json{
      {"version", 1},
      {"parents", Json::array()},
      {"message", "Syrnck Git assets v1 " + catalog.commit},
      {"timestamp", std::uint64_t{0}},
      {"files",
       files}}.dump();
  validate_object(Kind::revision, bytes(value));
  return Bytes(value.begin(), value.end());
}
ObjectKey bundle_key(const Catalog &catalog) {
  return {Kind::revision, object_id(Kind::revision, bundle_bytes(catalog))};
}
void verify_assets(const Store &store, const Catalog &catalog, OperationOptions operation) {
  std::set<ObjectKey> manifests;
  for (const auto &asset : catalog.assets) {
    throw_if_cancelled(operation.stop);
    auto metadata = store.metadata(Kind::manifest, asset.manifest);
    require(metadata["size"].get<std::uint64_t>() == asset.size, "pointer size mismatch");
    manifests.insert({Kind::manifest, asset.manifest});
  }
  (void)store.closure(std::vector<ObjectKey>(manifests.begin(), manifests.end()), true, operation.stop,
                      operation.progress);
}
ObjectKey prepare_bundle(const Store &store, const Catalog &catalog, OperationOptions operation) {
  auto payload = bundle_bytes(catalog);
  auto root = ObjectKey{Kind::revision, object_id(Kind::revision, payload)};
  auto references = store.refs();
  auto found = references.find("git-assets/" + catalog.commit);
  require(found == references.end() || found->second == root, "local Git catalog points to another object");
  verify_assets(store, catalog, operation);
  throw_if_cancelled(operation.stop);
  auto key = store.put(Kind::revision, payload).first;
  store.set_ref("git-assets/" + catalog.commit, key, operation.stop);
  return key;
}
} // namespace syrnck::git
