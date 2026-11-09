#include <syrnck/workspace.hpp>
#include <syrnck/watcher.hpp>
#include "working_file_stamp.hpp"
namespace syrnck {
struct WorkspaceCapture::Impl {
  struct Entry { std::string manifest, stamp; bool executable; };
  fs::path directory, storage;
  std::optional<std::string> context;
  std::string target, epoch, head;
  std::map<std::string, Entry> files;
  Impl(const fs::path &directory, const fs::path &storage)
      : directory(fs::canonical(directory)), storage(fs::canonical(storage)) {}
};
WorkspaceCapture::WorkspaceCapture(const fs::path &directory, const fs::path &storage)
    : impl_(std::make_unique<Impl>(directory, storage)) {}
WorkspaceCapture::~WorkspaceCapture() = default;
WorkspaceCapture::WorkspaceCapture(WorkspaceCapture &&) noexcept = default;
WorkspaceCapture &WorkspaceCapture::operator=(WorkspaceCapture &&) noexcept = default;
void WorkspaceCapture::invalidate() { impl_->files.clear(); impl_->head.clear(); }
Json WorkspaceCapture::capture(const Store &store, std::string_view branch, std::string_view message,
                               IngestOptions options, WorkspaceObservation *observation) {
  auto &state = *impl_;
  try {
    throw_if_cancelled(options.stop);
    require(store.root() == state.storage && (!state.context || *state.context == store.native_workspace_name()),
            "workspace capture belongs to another store or context");
    store.validate_native_workspace(state.directory);
    state.context = store.native_workspace_name();
    const auto refs = store.refs();
    const auto name = native_revision_reference(store, std::string(branch));
    const auto found = refs.find(name);
    const auto head = found == refs.end() ? std::string{} : found->second.id;
    const auto epoch = read_native_materialization(store, store.native_workspace_name(), refs).policy.identity();
    if (state.head != head || state.target != branch || state.epoch != epoch) invalidate();
    std::map<std::string, Impl::Entry> next;
    std::uint64_t read_bytes = 0, reused_bytes = 0, ingested_files = 0, reused_files = 0;
    const auto revision = commit_native_directory(store, state.directory, branch, message, options,
        [&](const NativeFile &file) {
      const auto before = working_file_stamp(file);
      const auto old = state.files.find(file.relative);
      if (before && before->reusable && old != state.files.end() && before->id == old->second.stamp &&
          file.executable == old->second.executable) {
        const auto after = working_file_stamp(file);
        require(after && after->id == before->id, "file changed during ingestion; retry commit");
        require(file.size <= UINT64_MAX - reused_bytes, "native comparison byte count overflow");
        reused_bytes += file.size; ++reused_files;
        next.emplace(file.relative, old->second);
        return Json{{"manifest", old->second.manifest}, {"executable", file.executable}};
      }
      const auto entry = ingest_native_file(store, file, options, "file changed during ingestion; retry commit");
      const auto after = working_file_stamp(file);
      if (before)
        require(after && after->id == before->id, "file changed during ingestion; retry commit");
      if (before && after)
        next.emplace(file.relative, Impl::Entry{entry.at("manifest").get<std::string>(), after->id, file.executable});
      require(file.size <= UINT64_MAX - read_bytes, "native comparison byte count overflow");
      read_bytes += file.size; ++ingested_files;
      return entry;
    });
    state.files = std::move(next);
    state.head = revision; state.target = branch; state.epoch = epoch;
    if (observation) observation->adopt_capture(store, state.directory, revision);
    return Json{{"version", 1}, {"revision", revision}, {"capture", Json{{"version", 1},
        {"ingested_files", ingested_files}, {"reused_files", reused_files},
        {"source_bytes_read", read_bytes}, {"source_bytes_reused", reused_bytes}}}};
  } catch (...) { invalidate(); if (observation) observation->invalidate(); throw; }
}
} // namespace syrnck
