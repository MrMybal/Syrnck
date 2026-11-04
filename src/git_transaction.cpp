#include "git_internal.hpp"
#include <syrnck/git_lfs.hpp>
#include <cstdio>
#include <cerrno>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#endif

namespace syrnck::git::detail {
#ifdef SYRNCK_TEST_GIT_TRANSACTION
// Defined only by the private probe; absent from production builds.
void test_transaction_acknowledgement();
#endif
namespace {
#ifdef _WIN32
using Native = HANDLE;
constexpr Native invalid = nullptr;
void close_native(Native value) {
  if (value)
    CloseHandle(value);
}
#else
using Native = int;
constexpr Native invalid = -1;
void close_native(Native value) {
  if (value >= 0)
    ::close(value);
}
class PipeSignal {
  sigset_t blocked_{}, previous_{};
  bool pending_ = false;

public:
  PipeSignal() {
    sigemptyset(&blocked_);
    sigaddset(&blocked_, SIGPIPE);
    require(pthread_sigmask(SIG_BLOCK, &blocked_, &previous_) == 0,
            "cannot configure Git reference transaction signals");
    sigset_t pending;
    pending_ = sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1;
  }
  ~PipeSignal() {
    (void)pthread_sigmask(SIG_SETMASK, &previous_, nullptr);
  }
  void broken_pipe() {
    sigset_t pending;
    if (!pending_ && sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1) {
      int signal = 0;
      (void)sigwait(&blocked_, &signal);
    }
  }
};
#endif
struct Owned {
  Native value = invalid;
  ~Owned() {
    reset();
  }
  void reset(Native replacement = invalid) {
    close_native(value);
    value = replacement;
  }
};
class Transaction {
  Owned input_, output_, child_input_, child_output_;
  std::unique_ptr<std::FILE, decltype(&std::fclose)> errors_{nullptr, &std::fclose};
#ifdef _WIN32
  Owned process_;
#else
  pid_t process_ = -1;
#endif
public:
  explicit Transaction(const fs::path &directory) {
    errors_.reset(std::tmpfile());
    require(bool(errors_), "cannot allocate Git reference transaction streams");
#ifdef _WIN32
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    require(CreatePipe(&child_input_.value, &input_.value, &security, 0) != 0 &&
                CreatePipe(&output_.value, &child_output_.value, &security, 0) != 0,
            "cannot allocate Git reference transaction streams");
    require(SetHandleInformation(input_.value, HANDLE_FLAG_INHERIT, 0) != 0 &&
                SetHandleInformation(output_.value, HANDLE_FLAG_INHERIT, 0) != 0,
            "cannot configure Git reference transaction streams");
    auto error_handle = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(errors_.get())));
    require(SetHandleInformation(error_handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT) != 0,
            "cannot configure Git reference transaction streams");
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = child_input_.value;
    startup.hStdOutput = child_output_.value;
    startup.hStdError = error_handle;
    PROCESS_INFORMATION info{};
    std::wstring command = L"git --no-pager --no-replace-objects --no-lazy-fetch -c core.fsmonitor=false "
                           L"update-ref --no-deref --stdin";
    auto started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                  directory.empty() ? nullptr : directory.c_str(), &startup, &info);
    SetHandleInformation(error_handle, HANDLE_FLAG_INHERIT, 0);
    require(started != 0, "cannot start Git reference transaction");
    CloseHandle(info.hThread);
    process_.value = info.hProcess;
#else
    auto pipe = [](Owned &read, Owned &write) {
      int descriptors[2];
      require(::pipe(descriptors) == 0, "cannot allocate Git reference transaction streams");
      read.value = descriptors[0];
      write.value = descriptors[1];
      require(fcntl(read.value, F_SETFD, FD_CLOEXEC) == 0 && fcntl(write.value, F_SETFD, FD_CLOEXEC) == 0,
              "cannot configure Git reference transaction streams");
    };
    pipe(child_input_, input_);
    pipe(output_, child_output_);
    require(fcntl(fileno(errors_.get()), F_SETFD, FD_CLOEXEC) == 0,
            "cannot configure Git reference transaction streams");
    auto pid = fork();
    require(pid >= 0, "cannot start Git reference transaction");
    if (pid == 0) {
      if ((!directory.empty() && chdir(directory.c_str()) != 0) ||
          dup2(child_input_.value, STDIN_FILENO) < 0 || dup2(child_output_.value, STDOUT_FILENO) < 0 ||
          dup2(fileno(errors_.get()), STDERR_FILENO) < 0)
        _exit(127);
      close_native(child_input_.value);
      close_native(input_.value);
      close_native(output_.value);
      close_native(child_output_.value);
      execlp("git", "git", "--no-pager", "--no-replace-objects", "--no-lazy-fetch", "-c",
             "core.fsmonitor=false", "update-ref", "--no-deref", "--stdin", static_cast<char *>(nullptr));
      _exit(127);
    }
    process_ = pid;
#endif
    child_input_.reset();
    child_output_.reset();
  }
  ~Transaction() {
    (void)finish();
  }
  bool finish() noexcept {
    input_.reset();
    output_.reset();
#ifdef _WIN32
    if (!process_.value)
      return true;
    auto waited = WaitForSingleObject(process_.value, INFINITE);
    DWORD code = 1;
    auto ok = GetExitCodeProcess(process_.value, &code);
    process_.reset();
    return waited == WAIT_OBJECT_0 && ok && code == 0;
#else
    if (process_ < 0)
      return true;
    int status = 0;
    pid_t waited;
    do {
      waited = waitpid(process_, &status, 0);
    } while (waited < 0 && errno == EINTR);
    process_ = -1;
    return waited >= 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
  }
  void send(std::string_view command) {
#ifndef _WIN32
    PipeSignal signal;
#endif
    while (!command.empty()) {
#ifdef _WIN32
      DWORD count = 0;
      require(WriteFile(input_.value, command.data(), static_cast<DWORD>(command.size()), &count, nullptr) !=
                      0 &&
                  count > 0,
              "Git reference transaction write failed");
#else
      auto count = ::write(input_.value, command.data(), command.size());
      if (count < 0 && errno == EINTR)
        continue;
      if (count < 0 && errno == EPIPE)
        signal.broken_pipe();
      require(count > 0, "Git reference transaction write failed");
#endif
      command.remove_prefix(static_cast<std::size_t>(count));
    }
  }
  void receive(std::string_view expected) {
    std::string line;
    for (std::size_t total = 0; total < 16384; ++total) {
      char c = 0;
#ifdef _WIN32
      DWORD count = 0;
      require(ReadFile(output_.value, &c, 1, &count, nullptr) != 0 && count == 1,
              "Git reference transaction response missing");
#else
      auto count = ::read(output_.value, &c, 1);
      if (count < 0 && errno == EINTR) {
        --total;
        continue;
      }
      require(count == 1, "Git reference transaction response missing");
#endif
      line += c;
      require(line.size() <= 4096, "Git reference transaction response exceeds size limit");
      if (c == '\n') {
        if (line == expected)
          return;
        line.clear(); // Hooks can write diagnostic lines to stdout.
      }
    }
    throw Error("Git reference transaction response exceeds size limit");
  }
};
} // namespace
void create_branch(const fs::path &directory, const std::string &reference, const std::string &commit,
                   const std::function<void()> &guard, std::stop_token stop) {
  Transaction transaction(directory);
  transaction.send("start\n");
  transaction.receive("start: ok\n");
  transaction.send("create " + reference + ' ' + commit + "\nprepare\n");
  transaction.receive("prepare: ok\n");
  // The Git backend holds the ref lock. Old Git accepts a dangling symbolic ref
  // as an absent OID; inspect its actual type while that lock prevents writers.
  guard();
  throw_if_cancelled(stop);
  try {
    transaction.send("commit\n");
    transaction.receive("commit: ok\n");
#ifdef SYRNCK_TEST_GIT_TRANSACTION
    test_transaction_acknowledgement();
#endif
    require(transaction.finish(), "Git reference transaction did not finish");
  } catch (const std::exception &) {
    (void)transaction.finish();
    auto diagnostic = "Git LFS migration publication uncertain; inspect " + reference + " [" + commit + "]";
    throw LfsPublicationUncertain(diagnostic);
  }
}
} // namespace syrnck::git::detail
