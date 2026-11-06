#pragma once
#include <syrnck/core.hpp>
namespace syrnck {
// Persistent no-checkout bisection. The session pins its initial bad history;
// candidate testing/export and workspace changes remain explicit caller actions.
class NativeBisect {
  const Store &store_;

public:
  explicit NativeBisect(const Store &store) : store_(store) {}
  Json start(std::string_view good, std::string_view bad, OperationOptions = {}) const;
  Json status(OperationOptions = {}) const;
  Json mark(std::string_view result, std::string_view revision, std::string_view expected_state,
            OperationOptions = {}) const;
  Json reset(std::string_view expected_state, OperationOptions = {}) const;
};
} // namespace syrnck
