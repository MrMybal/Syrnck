// Test-only relay: give the CLI a private hidden console and deliver a real
// CTRL_BREAK_EVENT on demand. The production CLI has no testing back door.
#include <windows.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
namespace {
void check(bool value) {
  if (!value)
    throw std::runtime_error("console test relay failed");
}
struct Handle {
  HANDLE value = nullptr;
  ~Handle() {
    if (value && value != INVALID_HANDLE_VALUE)
      CloseHandle(value);
  }
};
BOOL WINAPI ignore_break(DWORD event) {
  return event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT;
}
std::wstring quote(const std::wstring &input) {
  std::wstring result = L"\"";
  std::size_t slashes = 0;
  for (auto c : input) {
    if (c == L'\\') {
      ++slashes;
      continue;
    }
    result.append(c == L'"' ? 2 * slashes + 1 : slashes, L'\\');
    slashes = 0;
    result += c;
  }
  result.append(2 * slashes, L'\\');
  return result + L'"';
}
} // namespace
int wmain(int argc, wchar_t **argv) {
  try {
    check(argc >= 3);
    check(SetConsoleCtrlHandler(ignore_break, TRUE) != 0);
    Handle job{CreateJobObjectW(nullptr, nullptr)};
    check(job.value != nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    check(SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) !=
          0);
    std::wstring command;
    for (int i = 2; i < argc; ++i) {
      if (!command.empty())
        command += L' ';
      command += quote(argv[i]);
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process{};
    check(CreateProcessW(argv[2], command.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED, nullptr, nullptr,
                         &startup, &process) != 0);
    Handle child{process.hProcess}, thread{process.hThread};
    if (!AssignProcessToJobObject(job.value, child.value)) {
      TerminateProcess(child.value, 1);
      throw std::runtime_error("cannot assign CLI test job");
    }
    check(ResumeThread(thread.value) != static_cast<DWORD>(-1));
    bool sent = false;
    while (WaitForSingleObject(child.value, 20) == WAIT_TIMEOUT) {
      if (!sent && std::filesystem::exists(argv[1])) {
        check(GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, 0) != 0);
        sent = true;
      }
    }
    DWORD status = 1;
    check(GetExitCodeProcess(child.value, &status) != 0);
    return static_cast<int>(status);
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
