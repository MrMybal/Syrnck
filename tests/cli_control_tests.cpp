#include "cli_control.hpp"
#include <chrono>
#include <csignal>
#include <iostream>
#include <sstream>
#include <thread>
using namespace syrnck;
namespace {
void check(bool condition) {
  require(condition, "CLI control test failed");
}
volatile std::sig_atomic_t restored_handler_called = 0;
void previous_handler(int) {
  restored_handler_called = 1;
}
void await_stop(const cli::OperationControl &control) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!control.token().stop_requested() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  check(control.interrupted() && control.token().stop_requested());
}
} // namespace
int main() {
  try {
    auto original = std::signal(SIGINT, previous_handler);
    {
      cli::OperationControl control(Language::english, cli::ProgressMode::none);
      check(!control.interrupted() && !control.token().stop_requested());
      bool rejected = false;
      try {
        cli::OperationControl second(Language::english, cli::ProgressMode::none);
      } catch (const Error &) {
        rejected = true;
      }
      check(rejected);
      std::raise(SIGINT);
      await_stop(control);
      bool cancelled = false;
      try {
        control.report({});
      } catch (const Cancelled &) {
        cancelled = true;
      }
      check(cancelled);
    }
    std::raise(SIGINT);
    check(restored_handler_called == 1);
    std::signal(SIGINT, original);
    {
      cli::OperationControl control(Language::english, cli::ProgressMode::none);
      check(!control.interrupted() && !control.token().stop_requested());
      std::raise(SIGTERM);
      await_stop(control);
    }
    std::ostringstream captured;
    struct Redirect {
      std::streambuf *previous;
      ~Redirect() {
        std::cerr.rdbuf(previous);
      }
    } redirect{std::cerr.rdbuf(captured.rdbuf())};
    {
      cli::OperationControl control(Language::french, cli::ProgressMode::json);
      ProgressEvent event{ProgressPhase::ingesting, "asset\n\".bin", 0, 17};
      control.report(event);
      control.report(event); // throttled
      event.bytes = 17;
      event.complete = true;
      control.report(event); // completion is never throttled
    }
    std::istringstream lines(captured.str());
    std::string line;
    check(static_cast<bool>(std::getline(lines, line)));
    auto first = Json::parse(line);
    check(first["phase"] == "ingesting" && first["path"] == "asset\n\".bin" && first["total_bytes"] == 17 &&
          first["bytes"] == 0 && !first["complete"].get<bool>());
    check(static_cast<bool>(std::getline(lines, line)) && Json::parse(line)["complete"] == true);
    check(!std::getline(lines, line));
    captured.str({});
    {
      cli::OperationControl control(Language::french, cli::ProgressMode::text);
      control.report({ProgressPhase::reconstructing, "a\nb", 4, {}, true});
    }
    check(captured.str().find("Progression : Reconstruction") != std::string::npos);
    check(captured.str().find("4 octets") != std::string::npos);
    check(captured.str().find("\"a\\nb\"") != std::string::npos);
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
