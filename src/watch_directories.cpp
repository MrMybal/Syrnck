#include "watch_directories.hpp"
#include "native_workspace.hpp"
#include <iterator>
namespace syrnck {
namespace {
bool below(const fs::path &path, const fs::path &root) {
  auto p = path.begin();
  for (auto r = root.begin(); r != root.end(); ++r, ++p)
    if (p == path.end() || *p != *r) return false;
  return true;
}
} // namespace
WatchDirectories::WatchDirectories(fs::path root, fs::path storage, std::size_t limit)
    : root_(std::move(root)), storage_(std::move(storage)), limit_(limit) {
  require(limit_ >= 1 && limit_ <= 65536, "invalid workspace watcher limits");
  require(root_ != storage_ && below(storage_, root_), "native store must be below watched workspace");
}
void WatchDirectories::clear() {
  descriptors_.clear();
  paths_.clear();
  budget_ = 0;
}
const std::string *WatchDirectories::find(int descriptor) const {
  const auto row = descriptors_.find(descriptor);
  return row == descriptors_.end() ? nullptr : &row->second;
}
bool WatchDirectories::start(const Add &add, std::stop_token stop) {
  require(paths_.empty(), "directory watches must be rebuilt after registration failure");
  if (!tree(root_, add, stop)) return false;
  const auto relative = utf8(storage_.lexically_relative(root_));
  // The actual Store still needs its own refs/config watch when it is below
  // an excluded .git/.syrnck directory. Never enumerate its contents.
  return paths_.contains(relative) || tree(storage_, add, stop);
}
bool WatchDirectories::extend(std::string_view relative, const Add &add, std::stop_token stop) {
  throw_if_cancelled(stop);
  const auto path = path_from_utf8(relative);
  require(!relative.empty() && !path.is_absolute() && !path.has_root_name() && !path.has_root_directory() &&
              utf8(path) == relative,
          "invalid watched directory path");
  for (const auto &component : path)
    require(!component.empty() && component != "." && component != "..", "invalid watched directory path");
  auto directory = root_;
  for (const auto &component : path) {
    throw_if_cancelled(stop);
    directory /= component;
    // Check every ancestor; IN_DONT_FOLLOW only protects the final component.
    const auto status = fs::symlink_status(directory);
    if (!fs::is_directory(status) || native_link_like(directory, status)) return false;
    if (below(directory, storage_)) return directory == storage_ && tree(storage_, add, stop);
    if (component == ".git" || component == ".syrnck") return false;
  }
  return tree(directory, add, stop);
}
bool WatchDirectories::tree(const fs::path &initial, const Add &add, std::stop_token stop) {
  struct Pending { fs::path path; std::string relative; std::size_t depth; bool fresh; };
  std::vector<Pending> pending;
  std::set<std::string> scheduled;
  std::size_t reserved_count = 0, reserved_bytes = 0;
  const auto enqueue = [&](const fs::path &directory) {
    const auto local = directory == root_ ? fs::path{} : directory.lexically_relative(root_);
    const auto relative = utf8(local);
    const auto depth = static_cast<std::size_t>(std::distance(local.begin(), local.end()));
    if (depth > 128 || !scheduled.insert(relative).second) return false;
    const bool fresh = !paths_.contains(relative);
    if (fresh) {
      const auto cost = relative.size() + 64;
      if (paths_.size() + reserved_count == limit_ || cost > max_object_size - budget_ - reserved_bytes) return false;
      ++reserved_count;
      reserved_bytes += cost;
    }
    pending.push_back({directory, relative, depth, fresh});
    return true;
  };
  if (!enqueue(initial)) return false;
  while (!pending.empty()) {
    throw_if_cancelled(stop);
    const auto current = std::move(pending.back());
    pending.pop_back();
    const auto status = fs::symlink_status(current.path);
    if (!fs::is_directory(status) || native_link_like(current.path, status)) return false;
    // Arm before enumerating. Files created before registration are covered by
    // the caller's subsequent complete observation of this subtree; later
    // writes remain in the same native queue. No global queue is discarded.
    const auto descriptor = add(current.path);
    throw_if_cancelled(stop);
    if (descriptor < 0) return false;
    if (const auto old = descriptors_.find(descriptor); old != descriptors_.end() && old->second != current.relative)
      return false;
    if (const auto old = paths_.find(current.relative); old != paths_.end() && old->second != descriptor)
      return false;
    if (current.fresh) {
      --reserved_count;
      reserved_bytes -= current.relative.size() + 64;
      budget_ += current.relative.size() + 64;
      descriptors_.emplace(descriptor, current.relative);
      paths_.emplace(current.relative, descriptor);
    }
    if (current.path == storage_) continue;
    for (const auto &entry : fs::directory_iterator(current.path)) {
      throw_if_cancelled(stop);
      const auto child_status = entry.symlink_status();
      if (!fs::is_directory(child_status) || native_link_like(entry.path(), child_status)) continue;
      const auto path = entry.path();
      const auto name = utf8(path.filename());
      if (path != storage_ && (name == ".git" || name == ".syrnck" || below(path, storage_))) continue;
      if (current.depth == 128 || !enqueue(path)) return false;
    }
  }
  return true;
}
} // namespace syrnck
