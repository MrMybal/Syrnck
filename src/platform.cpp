#include "internal.hpp"
#include "io_checkpoint.hpp"
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <system_error>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#include <bcrypt.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/random.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif
namespace syrnck {
fs::path executable_path() {
#ifdef _WIN32
  std::wstring value(32768, L'\0');
  auto n = GetModuleFileNameW(nullptr, value.data(), static_cast<DWORD>(value.size()));
  require(n > 0 && n < value.size(), "cannot locate executable");
  value.resize(n);
  return fs::canonical(value);
#elif defined(__APPLE__)
  std::uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string value(size, '\0');
  require(_NSGetExecutablePath(value.data(), &size) == 0, "cannot locate executable");
  return fs::canonical(value.c_str());
#else
  return fs::canonical("/proc/self/exe");
#endif
}
std::string utf8(const fs::path &p) {
  auto s = p.generic_u8string();
  return {reinterpret_cast<const char *>(s.data()), s.size()};
}
fs::path path_from_utf8(std::string_view s) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t *>(s.data()), s.size()));
}
Bytes random_bytes(std::size_t size) {
  Bytes out(size);
#ifdef _WIN32
  require(size <= ULONG_MAX, "random request too large");
  require(BCryptGenRandom(nullptr, out.data(), static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG) ==
              0,
          "OS random generator failed");
#elif defined(__linux__)
  for (std::size_t i = 0; i < size;) {
    auto n = ::getrandom(out.data() + i, size - i, 0);
    if (n < 0 && errno == EINTR)
      continue;
    require(n > 0, "OS random generator failed");
    i += static_cast<std::size_t>(n);
  }
#else
  arc4random_buf(out.data(), size);
#endif
  return out;
}
Bytes read_bytes(const fs::path &p, std::size_t limit) {
  std::ifstream in(p, std::ios::binary);
  require(in.is_open(), "cannot open file: " + utf8(p));
  auto size = fs::file_size(p);
  require(size <= limit, "file exceeds size limit");
  Bytes out(static_cast<std::size_t>(size));
  in.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(out.size()));
  require(static_cast<std::size_t>(in.gcount()) == out.size(), "file changed or failed during read");
  return out;
}
static void sync_file(const fs::path &p) {
#ifdef _WIN32
  auto h = CreateFileW(p.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
  require(h != INVALID_HANDLE_VALUE, "cannot open file for flush");
  auto ok = FlushFileBuffers(h);
  CloseHandle(h);
  require(ok != 0, "file flush failed");
#else
  int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
  require(fd >= 0, "cannot open file for flush");
  int result = ::fsync(fd);
  ::close(fd);
  require(result == 0, "file flush failed");
#endif
}
void atomic_stream(const fs::path &destination, const std::function<void(std::ostream &)> &writer,
                   bool replace) {
  auto parent = destination.parent_path();
  if (parent.empty())
    parent = ".";
  fs::create_directories(parent);
  auto temp = parent / path_from_utf8(".tmp" + hex(random_bytes(16)));
  struct Cleanup {
    fs::path p;
    ~Cleanup() {
      std::error_code ec;
      fs::remove(p, ec);
    }
  } cleanup{temp};
#ifdef _WIN32
  auto h = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  require(h != INVALID_HANDLE_VALUE, "cannot create temporary file");
  CloseHandle(h);
#else
  int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  require(fd >= 0, "cannot create temporary file");
  ::close(fd);
#endif
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    require(out.is_open(), "cannot write temporary file");
    SYRNCK_IO_CHECKPOINT("atomic.temporary_opened", destination);
    writer(out);
    SYRNCK_IO_CHECKPOINT("atomic.writer_finished", destination);
    out.flush();
    require(out.good(), "file write failed");
  }
  sync_file(temp);
  SYRNCK_IO_CHECKPOINT("atomic.file_synced", destination);
#ifdef _WIN32
  DWORD flags = MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0);
  require(MoveFileExW(temp.c_str(), destination.c_str(), flags) != 0,
          "cannot publish file (destination may already exist)");
  SYRNCK_IO_CHECKPOINT("atomic.published", destination);
#else
  if (replace)
    require(::rename(temp.c_str(), destination.c_str()) == 0, "cannot replace file");
  else {
    require(::link(temp.c_str(), destination.c_str()) == 0,
            "cannot publish file (destination may already exist)");
    SYRNCK_IO_CHECKPOINT("atomic.linked", destination);
    require(::unlink(temp.c_str()) == 0, "cannot unlink temporary file");
  }
  SYRNCK_IO_CHECKPOINT("atomic.published", destination);
  int dir = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  require(dir >= 0, "cannot open directory for flush");
  int ok = ::fsync(dir);
  ::close(dir);
  require(ok == 0, "directory flush failed");
#endif
  SYRNCK_IO_CHECKPOINT("atomic.durable", destination);
}
void atomic_write(const fs::path &p, std::span<const std::uint8_t> b, bool replace) {
  atomic_stream(
      p,
      [&](std::ostream &out) {
        out.write(reinterpret_cast<const char *>(b.data()), static_cast<std::streamsize>(b.size()));
      },
      replace);
}
FileLock::FileLock(const fs::path &path) {
#ifdef _WIN32
  auto h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  require(h != INVALID_HANDLE_VALUE, "cannot open store lock");
  OVERLAPPED ov{};
  if (!LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov)) {
    CloseHandle(h);
    throw Error("store busy: another process holds its lock");
  }
  handle_ = reinterpret_cast<std::intptr_t>(h);
#else
  int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  require(fd >= 0, "cannot open store lock");
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    ::close(fd);
    throw Error("store busy: another process holds its lock");
  }
  handle_ = fd;
#endif
}
FileLock::~FileLock() {
#ifdef _WIN32
  if (handle_ != -1)
    CloseHandle(reinterpret_cast<HANDLE>(handle_));
#else
  if (handle_ != -1)
    ::close(static_cast<int>(handle_));
#endif
}
static std::string file_contents(std::FILE *f) {
  std::rewind(f);
  std::string out;
  std::array<char, 8192> buf{};
  for (;;) {
    auto n = std::fread(buf.data(), 1, buf.size(), f);
    out.append(buf.data(), n);
    if (n < buf.size()) {
      require(!std::ferror(f), "child output read failed");
      break;
    }
  }
  return out;
}
ProcessResult process(const std::vector<std::string> &args, const fs::path &cwd, std::string_view input) {
  require(!args.empty(), "empty child command");
  auto close_file = [](std::FILE *file) noexcept { (void)std::fclose(file); };
  using File = std::unique_ptr<std::FILE, decltype(close_file)>;
  File in(std::tmpfile(), close_file), out(std::tmpfile(), close_file), err(std::tmpfile(), close_file);
  require(in && out && err, "cannot allocate child streams");
  require(std::fwrite(input.data(), 1, input.size(), in.get()) == input.size(), "child input write failed");
  std::rewind(in.get());
#ifdef _WIN32
  auto quote = [](const std::string &arg) {
    auto value = path_from_utf8(arg).native();
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (wchar_t c : value) {
      if (c == L'\\') {
        ++slashes;
        continue;
      }
      if (c == L'\"') {
        result.append(slashes * 2 + 1, L'\\');
        result += c;
      } else {
        result.append(slashes, L'\\');
        result += c;
      }
      slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    result += L'\"';
    return result;
  };
  std::wstring command;
  for (const auto &arg : args) {
    if (!command.empty())
      command += L' ';
    command += quote(arg);
  }
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(in.get())));
  si.hStdOutput = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(out.get())));
  si.hStdError = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(err.get())));
  for (auto h : {si.hStdInput, si.hStdOutput, si.hStdError})
    require(SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT) != 0,
            "child handle setup failed");
  PROCESS_INFORMATION pi{};
  auto ok = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                           cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
  for (auto h : {si.hStdInput, si.hStdOutput, si.hStdError})
    SetHandleInformation(h, HANDLE_FLAG_INHERIT, 0);
  require(ok != 0, "cannot start child process");
  CloseHandle(pi.hThread);
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  int status = static_cast<int>(code);
#else
  std::vector<char *> argv;
  for (const auto &arg : args)
    argv.push_back(const_cast<char *>(arg.c_str()));
  argv.push_back(nullptr);
  int input_fd = fileno(in.get()), output_fd = fileno(out.get()), error_fd = fileno(err.get());
  for (int fd : {input_fd, output_fd, error_fd})
    require(::fcntl(fd, F_SETFD, FD_CLOEXEC) == 0, "child stream setup failed");
  pid_t pid = ::fork();
  require(pid >= 0, "cannot fork child process");
  if (pid == 0) {
    if (!cwd.empty() && ::chdir(cwd.c_str()) != 0)
      _exit(126);
    if (::dup2(input_fd, 0) < 0 || ::dup2(output_fd, 1) < 0 || ::dup2(error_fd, 2) < 0)
      _exit(126);
    ::execvp(argv[0], argv.data());
    _exit(127);
  }
  int wait = 0;
  while (::waitpid(pid, &wait, 0) < 0) {
    if (errno != EINTR)
      throw Error("child wait failed");
  }
  int status = WIFEXITED(wait) ? WEXITSTATUS(wait) : 128;
#endif
  return {status, file_contents(out.get()), file_contents(err.get())};
}
} // namespace syrnck
