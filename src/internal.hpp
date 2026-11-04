#pragma once
#include <functional>
#include <syrnck/core.hpp>
namespace syrnck {
Bytes read_bytes(const fs::path &, std::size_t limit = max_object_size + 1024 * 1024);
void atomic_write(const fs::path &, std::span<const std::uint8_t>, bool replace);
void atomic_stream(const fs::path &, const std::function<void(std::ostream &)> &, bool replace = false);
inline std::span<const std::uint8_t> bytes(std::string_view s) {
  return {reinterpret_cast<const std::uint8_t *>(s.data()), s.size()};
}
inline std::string text(std::span<const std::uint8_t> s) {
  return {reinterpret_cast<const char *>(s.data()), s.size()};
}
Json parse_json(std::span<const std::uint8_t>);
std::string ascii_lower(std::string s);
std::string portable_fold(std::string_view s);
std::size_t chunk_cut(std::span<const std::uint8_t>, const Chunking &);
struct ProcessResult {
  int status;
  std::string out, err;
};
ProcessResult process(const std::vector<std::string> &, const fs::path &cwd = {},
                      std::string_view input = {});
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
