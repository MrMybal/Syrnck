#pragma once
#include <syrnck/core.hpp>
#include <syrnck/i18n.hpp>

namespace syrnck::cli {
enum class ProgressMode { none, text, json };
// One command owns the process signal handlers. This is CLI plumbing only.
class OperationControl {
public:
  OperationControl(Language, ProgressMode);
  ~OperationControl();
  OperationControl(const OperationControl &) = delete;
  OperationControl &operator=(const OperationControl &) = delete;
  std::stop_token token() const;
  bool interrupted() const;
  void report(const ProgressEvent &);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace syrnck::cli
