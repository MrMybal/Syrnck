#include <syrnck/watcher.hpp>
#include "internal.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>
using namespace syrnck;
using namespace std::chrono_literals;
namespace {
template <class F> void fails(F f) {
  bool failed = false;
  try { f(); } catch (const Error &) { failed = true; }
  require(failed, "expected workspace watcher failure");
}
void write(const fs::path &path, std::string_view value) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << value;
  require(out.good(), "cannot write watcher fixture");
}
void create(const fs::path &project) { Store store(project / ".syrnck", {}, true); }
WorkspaceChanges path(WorkspaceWatcher &watcher, std::string_view wanted) {
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  do {
    auto batch = watcher.wait(50ms);
    if (std::find(batch.paths.begin(), batch.paths.end(), wanted) != batch.paths.end()) return batch;
  } while (std::chrono::steady_clock::now() < deadline);
  throw Error("filesystem notification path missing: " + std::string(wanted));
}
void drain(WorkspaceWatcher &watcher) {
  for (unsigned attempt = 0; attempt != 100; ++attempt)
    if (!watcher.wait(10ms).changed()) return;
  throw Error("watcher did not become idle");
}
void coalescing() {
  WorkspaceChanges batch{{"b", "a"}};
  batch.merge({{"b", "c"}, false, false, true}, 3);
  require(batch.paths == std::vector<std::string>({"a", "b", "c"}) && batch.polling && !batch.full_scan,
          "coalescing lost flags or duplicated path hints");
  batch.merge({{"d"}}, 3);
  require(batch.paths.empty() && batch.full_scan && batch.overflow, "coalescing path limit did not require audit");
  batch.merge({{"later"}});
  require(batch.paths.empty() && batch.overflow, "overflow resumed incomplete hints");
  WorkspaceChanges bytes;
  bytes.merge({{std::string(1024 * 1024, 'x')}});
  require(bytes.overflow && bytes.full_scan && bytes.paths.empty(), "coalescing did not enforce byte budget");
  WorkspaceChanges preexisting{{"one", "two"}};
  preexisting.merge({}, 1);
  require(preexisting.overflow && preexisting.paths.empty(), "coalescing ignored an already exceeded limit");
  fails([&] { preexisting.merge({}, 0); });
}
void native(const fs::path &project) {
  create(project);
  WorkspaceWatcher watcher(project, project / ".syrnck");
#ifdef _WIN32
  require(watcher.backend() == "windows", "Windows recursive notifications were not armed");
#elif defined(__linux__)
  require(watcher.backend() == "inotify", "Linux recursive notifications were not armed");
#else
  require(watcher.backend() == "polling", "portable polling fallback differs");
  return;
#endif
  { Store available(project / ".syrnck"); require(available.refs().empty(), "watcher locked or changed Store"); }
  write(project / "asset", "original");
  path(watcher, "asset");
  drain(watcher);
  write(project / path_from_utf8("Éditeur.cpp"), "source");
  path(watcher, "Éditeur.cpp");
  drain(watcher);
  fs::rename(project / "asset", project / "moved");
  auto batch = watcher.wait(50ms);
  std::set<std::string> names(batch.paths.begin(), batch.paths.end());
  for (unsigned i = 0; i != 40 && (!names.contains("asset") || !names.contains("moved")); ++i) {
    batch = watcher.wait(50ms);
    names.insert(batch.paths.begin(), batch.paths.end());
  }
  require(names.contains("asset") && names.contains("moved"), "rename sides were not observed");
  drain(watcher);
  fs::create_directories(project / "nested/deeper");
  path(watcher, "nested");
  drain(watcher);
  write(project / "nested/deeper/late", "late creation");
  path(watcher, "nested/deeper/late");
  drain(watcher);
  fs::rename(project / "nested", project / "renamed");
  path(watcher, "renamed");
  drain(watcher);
  write(project / "renamed/deeper/after-move", "still watched");
  path(watcher, "renamed/deeper/after-move");
  drain(watcher);
  fs::remove(project / "moved");
  path(watcher, "moved");
  drain(watcher);
  write(project / ".syrnck/working-index.json", "cache pointer");
  write(project / ".syrnck/objects/chunk/cache", "cache data");
  write(project / ".git/objects/private", "Git internals");
  require(!watcher.wait(100ms).changed(), "internal cache/Git activity caused a feedback notification");
  write(project / ".syrnck/refs.json", "{}");
  path(watcher, ".syrnck/refs.json");
  drain(watcher);
  watcher.invalidate();
  write(project / "after-invalidation", "event must remain queued");
  const auto full = watcher.wait(0ms);
  require(full.full_scan && !full.overflow, "explicit invalidation did not request a full scan");
  path(watcher, "after-invalidation");
}
void bounded(const fs::path &project) {
  create(project);
  WorkspaceWatcher watcher(project, project / ".syrnck", {false, 1});
  if (watcher.backend() == "polling") return;
  for (unsigned i = 0; i != 128; ++i) write(project / ("file-" + std::to_string(i)), "burst");
  bool overflow = false;
  for (unsigned i = 0; i != 20 && !overflow; ++i) {
    const auto batch = watcher.wait(20ms);
    require(batch.paths.size() <= 1, "notification path budget exceeded");
    overflow = batch.overflow;
    if (overflow) require(batch.full_scan && batch.paths.empty(), "overflow exposed incomplete change hints");
  }
  require(overflow, "burst did not exercise bounded notification overflow");
}
void cancellation(const fs::path &project, bool polling) {
  create(project);
  WorkspaceWatcher watcher(project, project / ".syrnck", {polling});
  if (polling) {
    const auto batch = watcher.wait(1ms);
    require(batch.polling && !batch.full_scan && batch.paths.empty(), "polling fallback contract differs");
  }
  std::stop_source stop;
  std::jthread cancel([&] { std::this_thread::sleep_for(35ms); stop.request_stop(); });
  const auto start = std::chrono::steady_clock::now();
  fails([&] { watcher.wait(60000ms, stop.get_token()); });
  require(std::chrono::steady_clock::now() - start < 1s, "watch wait ignored cancellation");
  fails([&] { watcher.wait(-1ms); });
  fails([&] { WorkspaceWatcher invalid(project, project / ".syrnck", {false, 0}); });
  std::stop_source pre;
  pre.request_stop();
  fails([&] { WorkspaceWatcher invalid(project, project / ".syrnck", {}, pre.get_token()); });
}
void replacement(const fs::path &project) {
  create(project);
  WorkspaceWatcher watcher(project, project / ".syrnck");
  auto old = project;
  old += path_from_utf8("-old");
  fs::rename(project, old);
  fs::create_directory(project);
  fails([&] { watcher.wait(0ms); });
}
void store_alias(const fs::path &project) {
  { Store store(project / "vault", {}, true); }
  if (fs::exists(project / "Vault")) return; // Same directory on insensitive filesystems.
  fs::create_directory(project / "Vault");
  WorkspaceWatcher watcher(project, project / "vault");
  if (watcher.backend() == "polling") return;
  write(project / "Vault/work", "tracked namespace outside the Store");
  path(watcher, "Vault/work");
}
#ifdef __linux__
void linux_additions(const fs::path &project) {
  create(project);
  write(project / "existing/before", "original directory");
  WorkspaceWatcher watcher(project, project / ".syrnck");
  require(watcher.backend() == "inotify", "Linux additions need native notifications");
  write(project / "new/deep/early", "present before descendant registration");
  auto batch = path(watcher, "new");
  require(!batch.full_scan && !batch.overflow && watcher.backend() == "inotify",
          "new directory discarded intact native notification continuity");
  drain(watcher);
  write(project / "new/deep/late", "registered descendant");
  require(!path(watcher, "new/deep/late").full_scan, "late descendant required a global rebuild");
  drain(watcher);
  write(project / "existing/after", "old descriptor still works");
  path(watcher, "existing/after");
  drain(watcher);
  auto outside = project;
  outside += "-outside";
  write(outside / "nested/early", "imported contents");
  fs::rename(outside, project / "imported");
  batch = path(watcher, "imported");
  require(!batch.full_scan && watcher.backend() == "inotify", "moved-in subtree discarded existing registrations");
  drain(watcher);
  write(project / "imported/nested/late", "registered moved-in descendant");
  path(watcher, "imported/nested/late");
  drain(watcher);
  fs::rename(project / "new", project / "renamed");
  require(path(watcher, "renamed").full_scan, "existing directory rename did not recover descriptor paths");
  drain(watcher);
  write(project / "renamed/deep/after", "renamed descendant still watched");
  path(watcher, "renamed/deep/after");
}
void linux_addition_quota(const fs::path &project) {
  create(project);
  WorkspaceWatcher watcher(project, project / ".syrnck", {false, 4096, 3});
  require(watcher.backend() == "inotify", "quota fixture did not initially arm native notifications");
  fs::create_directories(project / "new/deep");
  const auto batch = path(watcher, "new");
  require(batch.full_scan && watcher.backend() == "polling", "partial registration quota failure did not recover fully");
}
#endif
} // namespace
int main(int argc, char **argv) {
  if (argc == 3 && std::string_view(argv[1]) == "hold") {
    try {
      Store held(path_from_utf8(argv[2]));
      std::cout << "held" << std::endl;
      std::string line;
      return std::getline(std::cin, line) && line == "release" ? 0 : 1;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
  }
  const auto root = fs::temp_directory_path() / ("syrnck-watcher-" + hex(random_bytes(16)));
  struct Cleanup { fs::path root; ~Cleanup() { std::error_code error; fs::remove_all(root, error); } } cleanup{root};
  try {
    coalescing();
    native(root / "native");
    bounded(root / "bounded");
    cancellation(root / "cancel-native", false);
    cancellation(root / "cancel-polling", true);
    replacement(root / "replace");
    store_alias(root / "store-alias");
#ifdef __linux__
    linux_additions(root / "linux-additions");
    linux_addition_quota(root / "linux-quota");
#endif
    std::cout << "workspace notifications, bounds, cancellation and root identity passed\n";
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
