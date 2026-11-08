#include "native_directory_identity.hpp"
#ifdef _WIN32
#include <windows.h>
#elif defined(__linux__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
namespace syrnck {
std::optional<NativeDirectoryIdentity> native_directory_identity(const fs::path &path) {
#ifdef _WIN32
  const auto handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return {};
  struct Close { HANDLE handle; ~Close() { CloseHandle(handle); } } close{handle};
  FILE_ID_INFO identity{};
  FILE_ATTRIBUTE_TAG_INFO attributes{};
  if (!GetFileInformationByHandleEx(handle, FileIdInfo, &identity, sizeof(identity)) ||
      !GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &attributes, sizeof(attributes)) ||
      !(attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
      (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return {};
  return NativeDirectoryIdentity{"windows:" + std::to_string(identity.VolumeSerialNumber),
      hex(std::span<const std::uint8_t>(identity.FileId.Identifier, 16))};
#elif defined(__linux__) || defined(__APPLE__)
  const auto descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_DIRECTORY);
  if (descriptor < 0) return {};
  struct Close { int descriptor; ~Close() { ::close(descriptor); } } close{descriptor};
  struct stat status{};
  if (::fstat(descriptor, &status) != 0 || !S_ISDIR(status.st_mode)) return {};
  return NativeDirectoryIdentity{"posix:" + std::to_string(status.st_dev), std::to_string(status.st_ino)};
#else
  (void)path;
  return {};
#endif
}
} // namespace syrnck
