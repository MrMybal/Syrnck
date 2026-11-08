#pragma once
#include <chrono>
#include <filesystem>
#include <thread>
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
}
