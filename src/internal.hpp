#pragma once
#include <functional>
#include <syrnck/core.hpp>
#include "native_target.hpp"
namespace syrnck {
Bytes read_bytes(const fs::path &, std::size_t limit = max_object_size + 1024 * 1024);
inline void throw_if_cancelled(std::stop_token stop) {
  if (stop.stop_requested())
    throw Cancelled("operation cancelled");
}
inline thread_local unsigned progress_callback_depth = 0;
inline void invoke_progress(const ProgressCallback &callback, const ProgressEvent &event) {
  struct Guard {
    Guard() { ++progress_callback_depth; }
    ~Guard() { --progress_callback_depth; }
  } guard;
  callback(event);
}
inline void report_progress(const OperationOptions &options, const ProgressEvent &event) {
  throw_if_cancelled(options.stop);
  if (options.progress)
    invoke_progress(options.progress, event);
  throw_if_cancelled(options.stop);
}
void atomic_write(const fs::path &, std::span<const std::uint8_t>, bool replace, std::stop_token = {});
void atomic_stream(const fs::path &, const std::function<void(std::ostream &)> &, bool replace = false,
                   std::stop_token = {});
void atomic_directory(const fs::path &, const std::function<void(const fs::path &)> &,
                      const std::function<void()> &before_publish = {}, std::stop_token = {});
void make_executable(const fs::path &);
void move_exclusive(const fs::path &source, const fs::path &destination);
void require_no_native_checkout(const Store &);
void sync_directory(const fs::path &);
// Only call for unknown inventory names, never canonical metadata records.
bool transient_metadata_disappeared(const fs::path &, std::stop_token = {});
std::uint64_t unix_seconds();
inline std::span<const std::uint8_t> bytes(std::string_view s) {
  return {reinterpret_cast<const std::uint8_t *>(s.data()), s.size()};
}
inline std::string text(std::span<const std::uint8_t> s) {
  return {reinterpret_cast<const char *>(s.data()), s.size()};
}
Json parse_json(std::span<const std::uint8_t>);
bool is_utf8_text(std::span<const std::uint8_t>, OperationOptions);
Refs parse_refs(std::span<const std::uint8_t>, std::string_view invalid_document);
Json encode_refs(const Refs &);
std::string ascii_lower(std::string s);
std::string portable_fold(std::string_view s);
std::size_t chunk_cut(std::span<const std::uint8_t>, const Chunking &);
struct ProcessResult {
  int status;
  std::string out, err;
};
ProcessResult process(const std::vector<std::string> &, const fs::path &cwd = {}, std::string_view input = {},
                      std::size_t output_limit = 64 * 1024 * 1024);
class FileLock {
public:
  explicit FileLock(const fs::path &);
  ~FileLock();
  FileLock(const FileLock &) = delete;
  FileLock &operator=(const FileLock &) = delete;

private:
  std::intptr_t handle_ = -1;
};
fs::path executable_path();
} // namespace syrnck
