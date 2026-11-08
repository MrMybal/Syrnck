#include <syrnck/watcher.hpp>
#include "native_workspace.hpp"
#include "working_file_stamp.hpp"
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <cerrno>
#include <poll.h>
#include <sys/inotify.h>
#include "watch_directories.hpp"
#endif
#endif
namespace syrnck {
void WorkspaceChanges::merge(const WorkspaceChanges &other, std::size_t limit) {
  require(limit >= 1 && limit <= 65536, "watch path limit must be in 1..65536");
  full_scan |= other.full_scan;
  overflow |= other.overflow;
  polling |= other.polling;
  if (overflow) { paths.clear(); full_scan = true; return; }
  std::set<std::string> merged;
  std::size_t budget = 0;
  const auto add = [&](const std::string &path) {
    if (merged.contains(path)) return true;
    if (merged.size() == limit || path.size() + 64 > 1024 * 1024 - budget) return false;
    budget += path.size() + 64;
    merged.insert(path);
    return true;
  };
  const std::vector<std::string> *sources[]{&paths, &other.paths};
  for (const auto *source : sources)
    for (const auto &path : *source)
      if (!add(path)) { paths.clear(); full_scan = overflow = true; return; }
  paths.assign(merged.begin(), merged.end());
}
namespace {
bool below(const fs::path &path, const fs::path &root) {
  auto p = path.begin();
  for (auto r = root.begin(); r != root.end(); ++r, ++p)
    if (p == path.end() || *p != *r) return false;
  return true;
}
// Keep the directory object alive for the whole watch session, including the
// polling backend, so a deleted root's inode/file ID cannot be recycled beneath
// its identity check. This handle does not lock the Store or working files.
class RootAnchor {
#ifdef _WIN32
  HANDLE handle = INVALID_HANDLE_VALUE;
#else
  int fd = -1;
#endif
public:
  explicit RootAnchor(const fs::path &root) {
#ifdef _WIN32
    handle = CreateFileW(root.c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    require(handle != INVALID_HANDLE_VALUE, "cannot inspect watched workspace");
#else
    fd = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    require(fd >= 0, "cannot inspect watched workspace");
#endif
  }
  ~RootAnchor() {
#ifdef _WIN32
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
#else
    if (fd >= 0) ::close(fd);
#endif
  }
  RootAnchor(const RootAnchor &) = delete;
  RootAnchor &operator=(const RootAnchor &) = delete;
  std::string value() const {
#ifdef _WIN32
    FILE_ID_INFO id{};
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    const auto ok = GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id)) &&
                    GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &attributes, sizeof(attributes));
    require(ok && (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                !(attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT), "cannot inspect watched workspace");
    return std::to_string(id.VolumeSerialNumber) + ':' + hex(std::span<const std::uint8_t>(id.FileId.Identifier, 16));
#else
    struct stat status{};
    require(::fstat(fd, &status) == 0 && S_ISDIR(status.st_mode), "cannot inspect watched workspace");
    return std::to_string(status.st_dev) + ':' + std::to_string(status.st_ino);
#endif
  }
};
std::string root_identity(const fs::path &root) {
  return RootAnchor(root).value();
}
} // namespace
struct WorkspaceWatcher::Impl {
  fs::path root, store;
  std::string store_relative, identity, mode = "polling";
  std::optional<std::string> external_metadata;
#ifdef _WIN32
  std::string store_fold;
#endif
  WorkspaceWatchOptions options;
  bool invalidated = false;
  RootAnchor anchor;
#ifdef _WIN32
  HANDLE handle = INVALID_HANDLE_VALUE, event = nullptr;
  OVERLAPPED request{};
  std::array<std::uint32_t, 16384> buffer{};
  bool pending = false;
#elif defined(__linux__)
  int fd = -1;
  WatchDirectories directories;
#endif
  Impl(const fs::path &directory, const fs::path &storage, WorkspaceWatchOptions value)
      : root(fs::canonical(directory)), store(fs::canonical(storage)), options(value), anchor(root)
#if defined(__linux__) && !defined(_WIN32)
      , directories(root, store, value.max_directories, value.external_store)
#endif
  {
    require(options.max_paths >= 1 && options.max_paths <= 65536 && options.max_directories >= 1 &&
                options.max_directories <= 65536, "invalid workspace watcher limits");
    require(root != store && !below(root, store) && (below(store, root) || options.external_store),
            "native store must be below watched workspace");
    store_relative = below(store, root) ? utf8(store.lexically_relative(root)) : std::string{};
#ifdef _WIN32
    store_fold = portable_fold(store_relative);
#endif
    identity = anchor.value();
    if (store_relative.empty()) {
      external_metadata = metadata_stamp();
      if (!external_metadata) options.polling = true;
    }
  }
  std::optional<std::string> metadata_stamp() const {
    try {
      std::string result;
      for (const auto *name : {"refs.json", "config.json"}) {
        const auto path = store / name;
        const auto status = fs::symlink_status(path);
        if (!fs::is_regular_file(status) || native_link_like(path, status)) return {};
        const NativeFile file{path, name, fs::file_size(path), fs::last_write_time(path), false};
        const auto stamp = working_file_stamp(file);
        if (!stamp) return {};
        result += stamp->id + '\n';
      }
      return result;
    } catch (const Error &) { return {}; }
      catch (const fs::filesystem_error &) { return {}; }
  }
  bool metadata_changed() {
    if (!external_metadata) return false;
    const auto next = metadata_stamp();
    if (!next) {
      external_metadata.reset(); close_native(); mode = "polling";
      return true;
    }
    if (*next == *external_metadata) return false;
    external_metadata = next;
    return true;
  }
  ~Impl() { close_native(); }
  void close_native() noexcept {
#ifdef _WIN32
    if (handle != INVALID_HANDLE_VALUE) {
      if (pending) {
        CancelIoEx(handle, &request);
        DWORD ignored = 0;
        GetOverlappedResult(handle, &request, &ignored, TRUE);
        pending = false;
      }
      CloseHandle(handle);
      handle = INVALID_HANDLE_VALUE;
    }
    if (event) { CloseHandle(event); event = nullptr; }
#elif defined(__linux__)
    if (fd >= 0) { ::close(fd); fd = -1; }
    directories.clear();
#endif
  }
  bool interesting(std::string_view path, bool structural) const {
    std::size_t boundary = store_relative.size();
    bool private_path = path == store_relative || path.starts_with(store_relative + '/');
#ifdef _WIN32
    if (!private_path) {
      const auto folded = portable_fold(path);
      if (folded == store_fold || folded.starts_with(store_fold + '/')) {
        // Case-insensitive aliases are private only if they name this Store.
        // Distinct Vault/vault directories can exist on case-sensitive volumes.
        auto components = 1 + std::count(store_relative.begin(), store_relative.end(), '/');
        boundary = 0;
        while (components--) {
          const auto slash = path.find('/', boundary);
          if (!components || slash == path.npos) { boundary = slash == path.npos ? path.size() : slash; break; }
          boundary = slash + 1;
        }
        std::error_code error;
        private_path = fs::equivalent(root / path_from_utf8(path.substr(0, boundary)), store, error) && !error;
      }
    }
#endif
    if (private_path) {
      if (boundary == path.size()) return structural;
      const auto local = ascii_lower(std::string(path.substr(boundary + 1)));
      return local == "refs.json" || local == "config.json";
    }
    for (std::size_t start = 0; start < path.size();) {
      const auto slash = path.find('/', start);
      const auto component = path.substr(start, slash == path.npos ? path.size() - start : slash - start);
      if (component == ".git" || component == ".syrnck") return false;
      if (slash == path.npos) break;
      start = slash + 1;
    }
    return true;
  }
  void add(WorkspaceChanges &batch, std::set<std::string> &paths, std::size_t &budget,
           std::string path, bool structural) const {
    if (batch.overflow) return;
    try {
      if (!interesting(path, structural)) return;
      if (!is_utf8_text(bytes(path), {})) {
        paths.clear(); batch.overflow = batch.full_scan = true; return;
      }
    } catch (const Error &) {
      paths.clear(); batch.overflow = batch.full_scan = true; return;
    } catch (const fs::filesystem_error &) {
      paths.clear(); batch.overflow = batch.full_scan = true; return;
    }
    if (paths.contains(path)) return;
    if (paths.size() == options.max_paths || path.size() + 64 > 1024 * 1024 - budget) {
      paths.clear();
      batch.overflow = batch.full_scan = true;
      return;
    }
    budget += path.size() + 64;
    paths.insert(std::move(path));
  }
#ifdef _WIN32
  bool arm(std::stop_token) {
    ResetEvent(event);
    request = {};
    request.hEvent = event;
    const auto ok = ReadDirectoryChangesW(handle, buffer.data(), static_cast<DWORD>(sizeof(buffer)), TRUE,
        FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE |
        FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_ATTRIBUTES, nullptr, &request, nullptr);
    pending = ok || GetLastError() == ERROR_IO_PENDING;
    return pending;
  }
#elif defined(__linux__)
  int watch_directory(const fs::path &directory) const {
    return inotify_add_watch(fd, directory.c_str(), IN_CREATE | IN_DELETE | IN_MODIFY | IN_CLOSE_WRITE |
        IN_ATTRIB | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT |
        IN_ONLYDIR | IN_DONT_FOLLOW);
  }
  bool arm(std::stop_token stop) {
    fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0) return false;
    return directories.start([&](const fs::path &directory) { return watch_directory(directory); }, stop);
  }
  bool extend(const std::set<std::string> &additions, std::stop_token stop) {
    std::set<std::string> completed;
    for (const auto &relative : additions) {
      throw_if_cancelled(stop);
      bool covered = false;
      for (auto slash = relative.find('/'); slash != relative.npos; slash = relative.find('/', slash + 1))
        if (completed.contains(relative.substr(0, slash))) { covered = true; break; }
      if (covered) continue;
      if (!directories.extend(relative, [&](const fs::path &directory) { return watch_directory(directory); }, stop))
        return false;
      completed.insert(relative);
    }
    return true;
  }

#endif
  void start(std::stop_token stop = {}) {
    throw_if_cancelled(stop);
    close_native();
    mode = "polling";
    if (options.polling) return;
    try {
#ifdef _WIN32
      handle = CreateFileW(root.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
          nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      if (handle != INVALID_HANDLE_VALUE) {
        event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (event && arm(stop)) { mode = "windows"; return; }
      }
#elif defined(__linux__)
      if (arm(stop)) { mode = "inotify"; return; }
#endif
    } catch (const Cancelled &) {
      close_native();
      throw;
    } catch (const fs::filesystem_error &) {
      // A changing topology or unavailable registration falls back to polling;
      // the consumer still performs its complete initial observation.
    } catch (const Error &) {
    }
    close_native();
  }
  WorkspaceChanges wait(std::chrono::milliseconds timeout, std::stop_token stop) {
    throw_if_cancelled(stop);
    require(timeout.count() >= 0 && timeout.count() <= 60000, "watch wait must be in 0..60000 milliseconds");
    require(root_identity(root) == identity, "watched workspace identity changed; restart watcher");
    WorkspaceChanges batch;
    if (invalidated) { invalidated = false; batch.full_scan = true; return batch; }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
#if defined(_WIN32) || defined(__linux__)
    std::set<std::string> paths;
    std::size_t budget = 0;
#endif
    for (;;) {
      throw_if_cancelled(stop);
      if (metadata_changed()) { batch.full_scan = true; return batch; }
      const auto left = std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
      const auto tick = static_cast<unsigned>(std::clamp<std::int64_t>(left.count(), 0, 50));
      if (mode == "polling") {
        if (left.count() <= 0) { batch.polling = true; return batch; }
        std::this_thread::sleep_for(std::chrono::milliseconds(tick));
      }
#ifdef _WIN32
      else {
        const auto result = WaitForSingleObject(event, tick);
        if (result == WAIT_OBJECT_0) {
          DWORD count = 0;
          const auto ok = GetOverlappedResult(handle, &request, &count, FALSE);
          const auto error = ok ? ERROR_SUCCESS : GetLastError();
          pending = false;
          Bytes data;
          if (ok && count <= sizeof(buffer)) {
            const auto *begin = reinterpret_cast<const std::uint8_t *>(buffer.data());
            data.assign(begin, begin + count);
          }
          if (!arm(stop)) { close_native(); mode = "polling"; batch.full_scan = batch.overflow = true; }
          if (!ok || data.empty()) {
            batch.full_scan = batch.overflow = true;
            if (!ok && error != ERROR_NOTIFY_ENUM_DIR) { close_native(); mode = "polling"; }
          }
          for (std::size_t offset = 0; offset < data.size();) {
            constexpr auto header = offsetof(FILE_NOTIFY_INFORMATION, FileName);
            if (data.size() - offset < header) { batch.full_scan = batch.overflow = true; break; }
            DWORD next = 0, action = 0, length = 0;
            std::memcpy(&next, data.data() + offset, sizeof(DWORD));
            std::memcpy(&action, data.data() + offset + 4, sizeof(DWORD));
            std::memcpy(&length, data.data() + offset + 8, sizeof(DWORD));
            if (!length || length % sizeof(wchar_t) || length > data.size() - offset - header ||
                action < FILE_ACTION_ADDED || action > FILE_ACTION_RENAMED_NEW_NAME) {
              batch.full_scan = batch.overflow = true; break;
            }
            std::wstring name(length / sizeof(wchar_t), L'\0');
            std::memcpy(name.data(), data.data() + offset + header, length);
            try { add(batch, paths, budget, utf8(fs::path(name)), action != FILE_ACTION_MODIFIED); }
            catch (const fs::filesystem_error &) { batch.full_scan = batch.overflow = true; }
            if (!next) break;
            if (next < header + length || next > data.size() - offset || next % sizeof(DWORD)) {
              batch.full_scan = batch.overflow = true; break;
            }
            offset += next;
          }
          if (batch.overflow) paths.clear();
          batch.paths.assign(paths.begin(), paths.end());
          if (batch.changed()) return batch;
        } else if (result != WAIT_TIMEOUT) {
          close_native(); mode = "polling"; batch.full_scan = batch.overflow = true; return batch;
        }
      }
#elif defined(__linux__)
      else {
        pollfd ready{fd, POLLIN, 0};
        const auto result = ::poll(&ready, 1, static_cast<int>(tick));
        if (result < 0 && errno == EINTR) continue;
        if (result < 0 || (ready.revents & (POLLERR | POLLHUP | POLLNVAL))) {
          close_native(); mode = "polling"; batch.full_scan = batch.overflow = true; return batch;
        }
        if (result > 0) {
          std::array<std::uint8_t, 65536> data{};
          const auto count = ::read(fd, data.data(), data.size());
          if (count < 0 && (errno == EAGAIN || errno == EINTR)) continue;
          bool topology = false;
          std::set<std::string> additions;
          if (count <= 0) batch.full_scan = batch.overflow = true;
          for (std::size_t offset = 0; count > 0 && offset < static_cast<std::size_t>(count);) {
            if (static_cast<std::size_t>(count) - offset < sizeof(inotify_event)) {
              batch.full_scan = batch.overflow = true; break;
            }
            inotify_event entry{};
            std::memcpy(&entry, data.data() + offset, sizeof(entry));
            if (entry.len > static_cast<std::size_t>(count) - offset - sizeof(entry)) {
              batch.full_scan = batch.overflow = true; break;
            }
            if (entry.mask & IN_Q_OVERFLOW) batch.full_scan = batch.overflow = true;
            const auto found = directories.find(entry.wd);
            if (found) {
              const auto *begin = reinterpret_cast<const char *>(data.data() + offset + sizeof(entry));
              const auto *end = static_cast<const char *>(std::memchr(begin, 0, entry.len));
              if (entry.len && !end) { batch.full_scan = batch.overflow = true; break; }
              const std::string name = entry.len ? std::string(begin, end) : std::string{};
              auto path = *found;
              if (!name.empty()) path += (path.empty() ? "" : "/") + name;
              const bool structural = entry.mask & (IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO |
                                                     IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT | IN_IGNORED);
              if (interesting(path, structural)) {
                add(batch, paths, budget, path, structural);
                if ((entry.mask & IN_ISDIR) && structural) {
                  if (entry.mask & (IN_DELETE | IN_MOVED_FROM | IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT | IN_IGNORED))
                    topology = true;
                  else if (entry.mask & (IN_CREATE | IN_MOVED_TO)) additions.insert(path);
                }
                if (entry.mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT | IN_IGNORED)) {
                  topology = true; batch.full_scan = true;
                }
              }
            } else if (!(entry.mask & IN_Q_OVERFLOW)) batch.full_scan = batch.overflow = true;
            offset += sizeof(entry) + entry.len;
          }
          if (!topology && !batch.overflow && !additions.empty()) {
            try { topology = !extend(additions, stop); }
            catch (const Cancelled &) {
              // Partial registration cannot be resumed as an intact journal.
              close_native(); mode = "polling"; invalidated = true; throw;
            } catch (const Error &) { topology = true; }
              catch (const fs::filesystem_error &) { topology = true; }
          }
          if (topology || batch.overflow) {
            batch.full_scan = true;
            try { start(stop); }
            catch (...) { invalidated = true; throw; }
          }
          if (batch.overflow) paths.clear();
          batch.paths.assign(paths.begin(), paths.end());
          if (batch.changed()) return batch;
        }
      }
#endif
      if (std::chrono::steady_clock::now() >= deadline) {
        // A sleep/poll can overshoot its deadline. Polling still owes the
        // consumer a query hint even if the loop did not visit its next turn.
        if (mode == "polling") batch.polling = true;
        return batch;
      }
    }
  }
};
WorkspaceWatcher::WorkspaceWatcher(const fs::path &directory, const fs::path &store, WorkspaceWatchOptions options,
                                     std::stop_token stop)
    : impl_(std::make_unique<Impl>(directory, store, options)) { impl_->start(stop); }
WorkspaceWatcher::~WorkspaceWatcher() = default;
WorkspaceWatcher::WorkspaceWatcher(WorkspaceWatcher &&) noexcept = default;
WorkspaceWatcher &WorkspaceWatcher::operator=(WorkspaceWatcher &&) noexcept = default;
std::string_view WorkspaceWatcher::backend() const { return impl_->mode; }
WorkspaceChanges WorkspaceWatcher::wait(std::chrono::milliseconds timeout, std::stop_token stop) {
  return impl_->wait(timeout, stop);
}
void WorkspaceWatcher::invalidate() { impl_->invalidated = true; }
} // namespace syrnck
