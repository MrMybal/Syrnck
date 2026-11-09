#pragma once
#include "internal.hpp"
namespace syrnck {
inline std::string native_conflict_copy_path(const std::string &path, const std::string &revision) {
  const auto slash = path.rfind('/');
  const auto name = path.substr(slash == path.npos ? 0 : slash + 1);
  const auto dot = name.rfind('.');
  const bool extension = dot != name.npos && dot != 0 && name.size() - dot <= 16;
  auto label = name.substr(0, extension ? dot : name.size());
  if (label.size() > 64) {
    std::size_t length = 64;
    while ((static_cast<unsigned char>(label[length]) & 0xc0) == 0x80) --length;
    label.resize(length);
  }
  return (slash == path.npos ? std::string{} : path.substr(0, slash + 1)) + label +
      ".syrnck-conflict-" + revision + '-' + hash(bytes(path)).substr(0, 16) +
      (extension ? name.substr(dot) : std::string{});
}
}
