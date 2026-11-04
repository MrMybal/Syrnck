#include "git_internal.hpp"
#include <syrnck/git.hpp>
#include <algorithm>
#include <charconv>
#include <limits>
#include <sstream>

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
namespace {
std::uint64_t unsigned_number(std::string_view text) {
  std::uint64_t number = 0;
  auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
  require(!text.empty() && parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size(),
          "invalid Git object size");
  return number;
}
struct Entry {
  std::string path, oid;
  std::size_t size;
  bool executable;
};
} // namespace
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
  report_progress(operation, {ProgressPhase::scanning, result.commit});
  auto tree = detail::local_git(directory, {"ls-tree", "-r", "-l", "-z", "--full-tree", result.commit}, {},
                                64 * 1024 * 1024, operation.stop);
  require(tree.status == 0, "cannot enumerate Git tree");
  std::vector<Entry> candidates;
  std::map<std::string, std::size_t> sizes;
  std::size_t at = 0, entries = 0;
  while (at < tree.out.size()) {
    throw_if_cancelled(operation.stop);
    require(++entries <= 262144, "Git catalog entry limit exceeded");
    auto end = tree.out.find('\0', at), tab = tree.out.find('\t', at);
    require(end != tree.out.npos && tab != tree.out.npos && tab < end, "invalid Git tree response");
    std::istringstream fields(tree.out.substr(at, tab - at));
    std::string mode, type, oid, size, extra;
    require(bool(fields >> mode >> type >> oid >> size) && !(fields >> extra), "invalid Git tree response");
    detail::validate_oid(oid);
    if (type == "blob" && (mode == "100644" || mode == "100755")) {
      auto length = unsigned_number(size);
      if (length <= 1024) {
        candidates.push_back({tree.out.substr(tab + 1, end - tab - 1), oid, static_cast<std::size_t>(length),
                              mode == "100755"});
        auto [position, inserted] = sizes.emplace(oid, static_cast<std::size_t>(length));
        require(inserted || position->second == length, "inconsistent Git blob size");
      }
    }
    at = end + 1;
  }
  // Bound each cat-file exchange; large Git blobs never enter this process.
  std::map<std::string, std::optional<Pointer>> pointers;
  for (auto cursor = sizes.begin(); cursor != sizes.end();) {
    std::vector<std::pair<std::string, std::size_t>> batch;
    std::string input;
    while (cursor != sizes.end() && batch.size() < 128) {
      batch.push_back(*cursor);
      input += cursor->first + '\n';
      ++cursor;
    }
    auto output = detail::local_git(directory, {"cat-file", "--batch"}, input, 160 * 1024, operation.stop);
    require(output.status == 0, "cannot read Git pointer blobs");
    std::size_t offset = 0;
    for (const auto &[oid, length] : batch) {
      auto expected = oid + " blob " + std::to_string(length) + '\n';
      require(std::string_view(output.out).substr(offset).starts_with(expected),
              "invalid Git batch response");
      offset += expected.size();
      require(length < output.out.size() - offset && output.out[offset + length] == '\n',
              "invalid Git batch response");
      pointers.emplace(oid, parse_pointer(std::string_view(output.out).substr(offset, length)));
      offset += length + 1;
    }
    require(offset == output.out.size(), "trailing Git batch response");
    report_progress(operation, {ProgressPhase::scanning, result.commit, pointers.size(), sizes.size()});
  }
  for (const auto &entry : candidates)
    if (const auto &pointer = pointers.at(entry.oid))
      result.assets.push_back({entry.path, entry.oid, pointer->manifest, pointer->size, entry.executable});
  std::sort(result.assets.begin(), result.assets.end(),
            [](const auto &a, const auto &b) { return a.path < b.path; });
  report_progress(operation, {ProgressPhase::scanning, result.commit, pointers.size(), sizes.size(), true});
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
