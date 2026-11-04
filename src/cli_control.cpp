#include "cli_control.hpp"
#include "internal.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <iostream>
#include <mutex>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif

namespace syrnck::cli {
namespace {
std::atomic_flag signal_received = ATOMIC_FLAG_INIT;
std::atomic_flag controller_active = ATOMIC_FLAG_INIT;
void signal_handler(int signal) {
  // atomic_flag operations are guaranteed lock-free and signal-safe. Keep all
  // allocation, stop callbacks, diagnostics and IO on ordinary threads.
  signal_received.test_and_set(std::memory_order_relaxed);
  std::signal(signal, signal_handler);
}
#ifdef _WIN32
BOOL WINAPI console_handler(DWORD event) {
  if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT)
    return FALSE;
  signal_received.test_and_set(std::memory_order_relaxed);
  return TRUE;
}
#endif
std::string_view phase_name(ProgressPhase phase) {
  switch (phase) {
  case ProgressPhase::scanning:
    return "scanning";
  case ProgressPhase::ingesting:
    return "ingesting";
  case ProgressPhase::reconstructing:
    return "reconstructing";
  case ProgressPhase::publishing:
    return "publishing";
  }
  return "unknown";
}
std::string_view phase_label(ProgressPhase phase) {
  switch (phase) {
  case ProgressPhase::scanning:
    return "Scanning";
  case ProgressPhase::ingesting:
    return "Ingesting";
  case ProgressPhase::reconstructing:
    return "Reconstructing";
  case ProgressPhase::publishing:
    return "Publishing";
  }
  return "unknown";
}
} // namespace
struct OperationControl::Impl {
  Language language;
  ProgressMode mode;
  std::stop_source cancellation;
  using Handler = void (*)(int);
  Handler previous_int = SIG_ERR, previous_term = SIG_ERR;
#ifdef _WIN32
  bool console_installed = false;
#endif
  std::jthread monitor;
  bool printed = false;
  ProgressPhase previous_phase = ProgressPhase::ingesting;
  std::string previous_path;
  std::chrono::steady_clock::time_point previous_time{};

  Impl(Language language, ProgressMode mode) : language(language), mode(mode) {
    require(!controller_active.test_and_set(), "operation controller already active");
    signal_received.clear();
    try {
      previous_int = std::signal(SIGINT, signal_handler);
      require(previous_int != SIG_ERR, "cannot install interruption handler");
      previous_term = std::signal(SIGTERM, signal_handler);
      require(previous_term != SIG_ERR, "cannot install interruption handler");
#ifdef _WIN32
      console_installed = SetConsoleCtrlHandler(console_handler, TRUE) != 0;
      require(console_installed || GetLastError() == ERROR_INVALID_HANDLE,
              "cannot install interruption handler");
#endif
      monitor = std::jthread([this](std::stop_token done) {
        std::mutex mutex;
        std::unique_lock lock(mutex);
        std::condition_variable_any changed;
        while (!done.stop_requested() && !signal_received.test(std::memory_order_relaxed))
          changed.wait_for(lock, done, std::chrono::milliseconds(20),
                           [] { return signal_received.test(std::memory_order_relaxed); });
        if (signal_received.test(std::memory_order_relaxed))
          cancellation.request_stop();
      });
    } catch (...) {
      restore_handlers();
      throw;
    }
  }
  void restore_handlers() {
#ifdef _WIN32
    if (console_installed)
      SetConsoleCtrlHandler(console_handler, FALSE);
#endif
    if (previous_term != SIG_ERR)
      std::signal(SIGTERM, previous_term);
    if (previous_int != SIG_ERR)
      std::signal(SIGINT, previous_int);
    controller_active.clear();
  }
  ~Impl() {
    monitor.request_stop();
    if (monitor.joinable())
      monitor.join();
    restore_handlers();
  }
};
OperationControl::OperationControl(Language language, ProgressMode mode)
    : impl_(std::make_unique<Impl>(language, mode)) {}
OperationControl::~OperationControl() = default;
std::stop_token OperationControl::token() const {
  return impl_->cancellation.get_token();
}
bool OperationControl::interrupted() const {
  return signal_received.test(std::memory_order_relaxed);
}
void OperationControl::report(const ProgressEvent &event) {
  auto &state = *impl_;
  if (interrupted())
    state.cancellation.request_stop();
  throw_if_cancelled(token());
  if (state.mode == ProgressMode::none)
    return;
  auto now = std::chrono::steady_clock::now();
  if (state.printed && event.phase == state.previous_phase && event.path == state.previous_path &&
      !event.complete && now - state.previous_time < std::chrono::milliseconds(100))
    return;
  state.printed = true;
  state.previous_phase = event.phase;
  state.previous_path = event.path;
  state.previous_time = now;
  if (state.mode == ProgressMode::json) {
    Json line{{"phase", phase_name(event.phase)},
              {"path", event.path},
              {"bytes", event.bytes},
              {"total_bytes", event.total_bytes ? Json(*event.total_bytes) : Json(nullptr)},
              {"complete", event.complete},
              {"buffered_blocks", event.buffered_blocks}};
    std::cerr << line.dump() << '\n';
  } else {
    std::cerr << localize("Progress: ", state.language) << localize(phase_label(event.phase), state.language);
    if (!event.path.empty())
      std::cerr << ' ' << Json(event.path).dump();
    std::cerr << ' ' << event.bytes;
    if (event.total_bytes)
      std::cerr << '/' << *event.total_bytes;
    std::cerr << localize(" bytes", state.language);
    if (event.complete)
      std::cerr << localize(" complete", state.language);
    std::cerr << '\n';
  }
  require(std::cerr.good(), "progress output write failed");
}
} // namespace syrnck::cli
