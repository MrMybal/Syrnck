#include "native_checkout_layout.hpp"
namespace syrnck {
namespace {
using Names = std::map<std::string, std::pair<std::string, bool>>;
Names names(const NativeSnapshot &files, std::set<std::string> &directories, OperationOptions options,
            const std::set<std::string> &extra = {}) {
  Names result;
  std::size_t budget = 0;
  auto add = [&](const std::string &path, bool directory) {
    const auto fold = portable_fold(path);
    const auto [found, added] = result.emplace(fold, std::pair{path, directory});
    require(added || found->second == std::pair{path, directory}, "case-insensitive path collision");
    if (added) {
      const auto cost = path.size() + fold.size() + 64;
      require(cost <= max_object_size - budget, "native checkout inventory exceeds 16 MiB budget");
      budget += cost;
    }
    if (directory)
      directories.insert(path);
  };
  for (const auto &path : extra) {
    throw_if_cancelled(options.stop);
    validate_path(path);
    add(path, true);
    for (auto n = path.find('/'); n != path.npos; n = path.find('/', n + 1))
      add(path.substr(0, n), true);
  }
  for (const auto &[path, entry] : files) {
    (void)entry;
    throw_if_cancelled(options.stop);
    add(path, false);
    for (auto n = path.find('/'); n != path.npos; n = path.find('/', n + 1))
      add(path.substr(0, n), true);
  }
  return result;
}
bool displaced(const Names &other, const Names::value_type &entry) {
  const auto found = other.find(entry.first);
  return found != other.end() && found->second != entry.second;
}
void descendants(const std::set<std::string> &directories, const std::string &root,
                 std::set<std::string> &removed) {
  removed.insert(root);
  const auto prefix = root + '/';
  for (auto i = directories.lower_bound(prefix); i != directories.end() && i->starts_with(prefix); ++i)
    removed.insert(*i);
}
} // namespace
NativeCheckoutLayout::NativeCheckoutLayout(const NativeSnapshot &source, const NativeSnapshot &target,
                                           OperationOptions options,
                                           const std::set<std::string> &extra_source_directories) {
  const auto before = names(source, source_directories, options, extra_source_directories);
  const auto after = names(target, target_directories, options);
  for (const auto &entry : before) {
    throw_if_cancelled(options.stop);
    if (displaced(after, entry)) {
      transition = true;
      if (entry.second.second)
        descendants(source_directories, entry.second.first, remove_directories);
    }
  }
  for (const auto &entry : after) {
    throw_if_cancelled(options.stop);
    if (entry.second.second && displaced(before, entry))
      descendants(target_directories, entry.second.first, rollback_directories);
  }
  evacuated = remove_directories;
  for (const auto &[path, entry] : source) {
    (void)entry;
    if (!target.contains(path))
      evacuated.insert(path);
  }
}
} // namespace syrnck
