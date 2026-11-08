#include "native_workspace.hpp"
#include <blake3.h>
#include <deque>
#include <fstream>
namespace syrnck {
namespace {
using Content = NativeContent;
using File = NativeSnapshotFile;
using Snapshot = NativeSnapshot;
class ManifestSummaries {
  const Store &store_;
  std::map<std::string, Content> cache_;
  std::deque<std::string> order_;

public:
  explicit ManifestSummaries(const Store &store) : store_(store) {}
  Content get(const std::string &id) {
    if (auto found = cache_.find(id); found != cache_.end())
      return found->second;
    const auto manifest = store_.metadata(Kind::manifest, id);
    Content content{manifest["size"].get<std::uint64_t>(), manifest["file_hash"].get<std::string>()};
    if (cache_.size() == 1024) {
      cache_.erase(order_.front());
      order_.pop_front();
    }
    cache_.emplace(id, content);
    order_.push_back(id);
    return content;
  }
};
Snapshot snapshot(const Store &store, std::string_view id, ManifestSummaries &summaries,
                  const OperationOptions &options) {
  throw_if_cancelled(options.stop);
  const auto revision = store.metadata(Kind::revision, id);
  const auto &files = revision["files"];
  Snapshot result;
  std::uint64_t completed = 0;
  report_progress(options, {ProgressPhase::planning, id, 0, files.size()});
  for (const auto &[path, entry] : files.items()) {
    throw_if_cancelled(options.stop);
    auto manifest = entry["manifest"].get<std::string>();
    result.emplace_hint(result.end(), path,
                        File{manifest, summaries.get(manifest), entry["executable"].get<bool>()});
    if (++completed % 256 == 0)
      report_progress(options, {ProgressPhase::planning, path, completed, files.size()});
  }
  report_progress(options, {ProgressPhase::planning, id, completed, files.size(), true});
  return result;
}
Json changes(const Snapshot &source, const Snapshot &target, bool workspace,
             const OperationOptions &options) {
  std::uint64_t added = 0, removed = 0, modified = 0, unchanged = 0, processed = 0;
  const auto total = source.size() + target.size();
  Json result = Json::array();
  auto left = source.begin(), right = target.begin();
  report_progress(options, {ProgressPhase::planning, {}, 0, total});
  while (left != source.end() || right != target.end()) {
    throw_if_cancelled(options.stop);
    const bool have_left = left != source.end() && (right == target.end() || left->first <= right->first);
    const bool have_right = right != target.end() && (left == source.end() || right->first <= left->first);
    const auto &path = have_left ? left->first : right->first;
    const File *before = have_left ? &left->second : nullptr, *after = have_right ? &right->second : nullptr;
    const bool content_changed = !before || !after || before->content.size != after->content.size ||
                                 before->content.hash != after->content.hash;
    bool executable_changed = before && after && before->executable != after->executable;
#ifdef _WIN32
    if (workspace)
      executable_changed = false;
#endif
    const bool manifest_changed = !before || !after || before->manifest != after->manifest;
    if (content_changed || executable_changed || (!workspace && manifest_changed)) {
      const auto status = !before ? (workspace ? "untracked" : "added") : !after ? "removed" : "modified";
      added += !before;
      removed += !after;
      modified += before && after;
      Json row{{"path", path},
               {"status", status},
               {"source", before ? before->json() : Json{}},
               {"target", after ? after->json() : Json{}},
               {"content_changed", content_changed},
               {"executable_changed", executable_changed}};
      if (!workspace)
        row["manifest_changed"] = manifest_changed;
      result.push_back(std::move(row));
    } else
      ++unchanged;
    processed += have_left + have_right;
    if (processed % 256 == 0)
      report_progress(options, {ProgressPhase::planning, path, processed, total});
    if (have_left)
      ++left;
    if (have_right)
      ++right;
  }
  report_progress(options, {ProgressPhase::planning, {}, processed, total, true});
  return Json{{"counts", Json{{workspace ? "untracked" : "added", added},
                              {"removed", removed},
                              {"modified", modified},
                              {"unchanged", unchanged}}},
              {"changes", std::move(result)}};
}
} // namespace
NativeSnapshot read_native_snapshot(const Store &store, std::string_view revision, OperationOptions options) {
  ManifestSummaries summaries(store);
  return snapshot(store, revision, summaries, options);
}
Json compare_native_file_maps(const NativeSnapshot &source, const NativeSnapshot &target,
                              OperationOptions options) {
  return changes(source, target, false, options);
}
Json compare_native_workspace_maps(const NativeSnapshot &source, const NativeSnapshot &target,
                                   OperationOptions options) {
  return changes(source, target, true, options);
}
NativeContent hash_native_file(const NativeFile &file, Bytes &buffer, const OperationOptions &options) {
  std::ifstream input(file.path, std::ios::binary);
  require(input.is_open(), "cannot open source file");
  blake3_hasher hasher;
  blake3_hasher_init(&hasher);
  std::uint64_t read = 0;
  report_progress(options, {ProgressPhase::verifying, file.relative, 0, file.size});
  while (input) {
    throw_if_cancelled(options.stop);
    input.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    require(!input.bad() && (!input.fail() || input.eof()), "source file read failed");
    const auto count = static_cast<std::size_t>(input.gcount());
    require(count <= file.size - read, "file changed during native scan; retry operation");
    blake3_hasher_update(&hasher, buffer.data(), count);
    read += count;
    report_progress(options, {ProgressPhase::verifying, file.relative, read, file.size, input.eof()});
  }
  input.close();
  const auto status = fs::symlink_status(file.path);
  require(read == file.size && fs::is_regular_file(status) && fs::file_size(file.path) == file.size &&
              fs::last_write_time(file.path) == file.modified,
          "file changed during native scan; retry operation");
#ifndef _WIN32
  const bool executable =
      (status.permissions() & (fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec)) !=
      fs::perms::none;
  require(executable == file.executable, "file changed during native scan; retry operation");
#endif
  Key digest{};
  blake3_hasher_finalize(&hasher, digest.data(), digest.size());
  return {read, hex(digest)};
}
Json Store::status_directory(const fs::path &directory, const std::optional<std::string> &revision,
                             OperationOptions options) const {
  throw_if_cancelled(options.stop);
  ManifestSummaries summaries(*this);
  const auto baseline = revision ? snapshot(*this, *revision, summaries, options) : Snapshot{};
  Snapshot current;
  Bytes buffer(1024 * 1024);
  std::uint64_t hashed_bytes = 0;
  visit_native_files(
      directory, *this,
      [&](const NativeFile &file) {
        auto content = hash_native_file(file, buffer, options);
        require(content.size <= UINT64_MAX - hashed_bytes, "native comparison byte count overflow");
        hashed_bytes += content.size;
        current.emplace(file.relative, File{{}, std::move(content), file.executable});
      },
      options);
  auto result = changes(baseline, current, true, options);
  return Json{{"version", 1},
              {"base", revision ? Json(*revision) : Json{}},
              {"clean", result["changes"].empty()},
              {"tracked_files", baseline.size()},
              {"workspace_files", current.size()},
              {"hashed_bytes", hashed_bytes},
              {"counts", std::move(result["counts"])},
              {"changes", std::move(result["changes"])}};
}
Json Store::compare_revisions(std::string_view source, std::string_view target,
                              OperationOptions options) const {
  return compare_native_snapshots(*this, std::string(source), std::string(target), options);
}
Json compare_native_snapshots(const Store &store, const std::optional<std::string> &source,
                              const std::optional<std::string> &target, OperationOptions options) {
  throw_if_cancelled(options.stop);
  ManifestSummaries summaries(store);
  const auto before = source ? snapshot(store, *source, summaries, options) : Snapshot{};
  const auto other = source == target || !target ? Snapshot{} : snapshot(store, *target, summaries, options);
  const auto &after = source == target ? before : other;
  auto result = changes(before, after, false, options);
  return Json{{"version", 1},
              {"source", source ? Json(*source) : Json{}},
              {"target", target ? Json(*target) : Json{}},
              {"equal", result["changes"].empty()},
              {"source_files", before.size()},
              {"target_files", after.size()},
              {"counts", std::move(result["counts"])},
              {"changes", std::move(result["changes"])}};
}
} // namespace syrnck
