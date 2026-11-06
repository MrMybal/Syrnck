#pragma once
#include <syrnck/watcher.hpp>
#include "internal.hpp"
namespace syrnck {
// Directory bookkeeping shared by Linux's initial registration and additive
// subtree registration. The callback owns OS watches; false/exception requires
// closing the whole native instance and rebuilding before using more hints.
// There is deliberately no descriptor removal/relabeling or rename pairing.
class WatchDirectories {
public:
  using Add = std::function<int(const fs::path &)>;
  WatchDirectories(fs::path root, fs::path storage, std::size_t max_directories);
  bool start(const Add &, std::stop_token = {});
  bool extend(std::string_view relative, const Add &, std::stop_token = {});
  const std::string *find(int descriptor) const;
  std::size_t size() const { return paths_.size(); }
  void clear();
private:
  fs::path root_, storage_;
  std::size_t limit_, budget_ = 0;
  std::map<int, std::string> descriptors_;
  std::map<std::string, int> paths_;
  bool tree(const fs::path &, const Add &, std::stop_token);
};
} // namespace syrnck
