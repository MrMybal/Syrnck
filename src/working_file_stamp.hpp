#pragma once
#include "native_workspace.hpp"
namespace syrnck {
struct WorkingFileStamp {
  std::string id;
  bool reusable;
};
std::optional<WorkingFileStamp> working_file_stamp(const NativeFile &);
} // namespace syrnck
