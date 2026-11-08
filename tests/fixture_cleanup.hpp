#pragma once
#include <chrono>
#include <filesystem>
#include <thread>
#include <stdexcept>
namespace fixture_test {
// Only for fixture setup after its children have already been removed. Never
// removes children or retries a native engine operation. Windows can keep a
// deleted child pending until its last handle closes (DeleteFileW semantics):
// https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-deletefilew
inline bool remove_empty_directory(const std::filesystem::path &path,
    std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
#ifdef _WIN32
  const auto deadline = std::chrono::steady_clock::now() + timeout;
#else
  (void)timeout;
#endif
  for (;;) {
    std::error_code error;
    const bool removed = std::filesystem::remove(path, error);
    if (!error) return removed;
#ifdef _WIN32
    // ERROR_DIR_NOT_EMPTY only. Unexpected entries that persist beyond the
    // short deadline, access errors and every other failure still fail the test.
    if (error.value() == 145 && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }
#endif
    throw std::filesystem::filesystem_error("remove empty fixture directory", path, error);
  }
}
// Reset only one direct child of an explicitly owned fixture directory. This
// helper is never used by the engine, and cannot delete the owner or its Store.
inline void remove_owned_tree(const std::filesystem::path &path, const std::filesystem::path &owner,
    std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
  namespace fs = std::filesystem;
  const auto root = fs::canonical(owner);
  const auto absolute = fs::absolute(path).lexically_normal();
  // canonical also expands Windows short (8.3) parent names. Compare canonical
  // parents before constructing the exact target within this owned directory.
  if (fs::canonical(absolute.parent_path()) != root || absolute.filename() == ".syrnck")
    throw std::invalid_argument("fixture cleanup target must be a direct non-Store child of its owner");
  const auto target = root / absolute.filename();
  if (fs::exists(target) && fs::exists(root / ".syrnck") && fs::equivalent(target, root / ".syrnck"))
    throw std::invalid_argument("fixture cleanup cannot remove its Store");
#ifdef _WIN32
  const auto deadline = std::chrono::steady_clock::now() + timeout;
#else
  (void)timeout;
#endif
  for (;;) {
    std::error_code error;
    fs::remove_all(target, error);
    if (!error) return;
#ifdef _WIN32
    if ((error.value() == 32 || error.value() == 145) && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }
#endif
    throw fs::filesystem_error("remove owned fixture tree", target, error);
  }
}

}
