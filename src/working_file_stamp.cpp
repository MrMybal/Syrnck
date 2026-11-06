#include "working_file_stamp.hpp"
#include <chrono>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/magic.h>
#include <sys/vfs.h>
#elif defined(__APPLE__)
#include <sys/mount.h>
#endif
#endif
namespace syrnck {
std::optional<WorkingFileStamp> working_file_stamp(const NativeFile &file) {
#ifdef _WIN32
  // Do not share write access: a writer can delay timestamps until its handle
  // closes. Such files must bypass reuse even when their size/mtime still match.
  // READ_DATA is intentional: attribute-only opens bypass sharing checks.
  const auto handle = CreateFileW(file.path.c_str(), FILE_READ_DATA | FILE_READ_ATTRIBUTES,
                                  FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (handle == INVALID_HANDLE_VALUE)
    return {};
  struct Close {
    HANDLE handle;
    ~Close() { CloseHandle(handle); }
  } close{handle};
  FILE_ID_INFO identity{};
  FILE_BASIC_INFO basic{};
  FILE_STANDARD_INFO standard{};
  wchar_t filesystem[32]{};
  std::wstring final_path(32768, L'\0');
  const auto length = GetFinalPathNameByHandleW(handle, final_path.data(),
                                              static_cast<DWORD>(final_path.size()), FILE_NAME_OPENED);
  if (!length || length >= final_path.size() || length < 7 || final_path.substr(0, 4) != L"\\\\?\\" ||
      final_path[5] != L':' || final_path[6] != L'\\')
    return {}; // Includes UNC paths and remote shares.
  const auto drive = final_path.substr(4, 3);
  if (GetDriveTypeW(drive.c_str()) != DRIVE_FIXED ||
      !GetVolumeInformationByHandleW(handle, nullptr, 0, nullptr, nullptr, nullptr, filesystem, 32) ||
      (std::wstring_view(filesystem) != L"NTFS" && std::wstring_view(filesystem) != L"ReFS") ||
      !GetFileInformationByHandleEx(handle, FileIdInfo, &identity, sizeof(identity)) ||
      !GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic)) ||
      !GetFileInformationByHandleEx(handle, FileStandardInfo, &standard, sizeof(standard)) ||
      standard.Directory || standard.DeletePending || standard.EndOfFile.QuadPart < 0 ||
      static_cast<std::uint64_t>(standard.EndOfFile.QuadPart) != file.size ||
      (basic.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE)))
    return {};
  FILETIME clock{};
  GetSystemTimeAsFileTime(&clock);
  const auto now = (static_cast<std::uint64_t>(clock.dwHighDateTime) << 32) | clock.dwLowDateTime;
  constexpr std::uint64_t guard = 2 * 10000000;
  const bool stable = basic.ChangeTime.QuadPart > 0 && basic.LastWriteTime.QuadPart > 0 && now > guard &&
                      static_cast<std::uint64_t>(basic.ChangeTime.QuadPart) < now - guard &&
                      static_cast<std::uint64_t>(basic.LastWriteTime.QuadPart) < now - guard;
  const auto value = Json::array({identity.VolumeSerialNumber,
                                 hex(std::span<const std::uint8_t>(identity.FileId.Identifier, 16)),
                                 basic.ChangeTime.QuadPart, basic.LastWriteTime.QuadPart,
                                 standard.EndOfFile.QuadPart, basic.FileAttributes}).dump();
  return WorkingFileStamp{hash(bytes(value)), stable};
#elif defined(__linux__) || defined(__APPLE__)
  const auto fd = ::open(file.path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0)
    return {};
  struct Close {
    int fd;
    ~Close() { ::close(fd); }
  } close{fd};
  struct stat status{};
  struct statfs filesystem{};
  if (::fstat(fd, &status) != 0 || ::fstatfs(fd, &filesystem) != 0 || !S_ISREG(status.st_mode) ||
      status.st_size < 0 || static_cast<std::uint64_t>(status.st_size) != file.size)
    return {};
#ifdef __linux__
  if (filesystem.f_type != EXT4_SUPER_MAGIC && filesystem.f_type != XFS_SUPER_MAGIC &&
      filesystem.f_type != BTRFS_SUPER_MAGIC && filesystem.f_type != TMPFS_MAGIC)
    return {};
  const auto modified = status.st_mtim, changed = status.st_ctim;
#else
  if (std::string_view(filesystem.f_fstypename) != "apfs" || !(filesystem.f_flags & MNT_LOCAL))
    return {};
  const auto modified = status.st_mtimespec, changed = status.st_ctimespec;
#endif
  const bool executable = (status.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0;
  if (executable != file.executable)
    return {};
  const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch()).count();
  const auto value = Json::array({status.st_dev, status.st_ino, status.st_size, status.st_mode,
                                 modified.tv_sec, modified.tv_nsec, changed.tv_sec, changed.tv_nsec}).dump();
  return WorkingFileStamp{hash(bytes(value)), modified.tv_sec > 0 && changed.tv_sec > 0 &&
                                               modified.tv_sec < now - 2 && changed.tv_sec < now - 2};
#else
  (void)file;
  return {};
#endif
}
} // namespace syrnck
