#include "watch_directories.hpp"
#include <iostream>
using namespace syrnck;
namespace {
void write(const fs::path &path, std::string_view value) {
  fs::create_directories(path.parent_path());
  atomic_write(path, bytes(value), true);
}
template<class F> void fails(F action) {
  bool rejected = false;
  try { action(); } catch (const Error &) { rejected = true; }
  require(rejected, "expected directory registration rejection");
}
struct Kernel {
  fs::path root;
  std::map<std::string, int> watches;
  std::vector<std::string> calls;
  std::function<void(std::string_view)> during_add;
  int add(const fs::path &path) {
    const auto relative = path == root ? std::string{} : utf8(path.lexically_relative(root));
    calls.push_back(relative);
    const auto [found, fresh] = watches.try_emplace(relative, static_cast<int>(watches.size()) + 1);
    (void)fresh;
    if (during_add) during_add(relative);
    return found->second;
  }
};
void additions(const fs::path &project) {
  const auto storage = project / "vault";
  { Store store(storage, {}, true); }
  write(project / "existing/nested/file", "baseline");
  write(project / ".git/objects/hidden/file", "Git private data");
  write(storage / "objects/private/nested/file", "Store private data");
  Kernel kernel{fs::canonical(project), {}, {}, {}};
  WatchDirectories registry(kernel.root, fs::canonical(storage), 128);
  const auto add = [&](const fs::path &path) { return kernel.add(path); };
  require(registry.start(add), "initial directory registration failed");
  require(registry.size() == 4 && kernel.watches.contains("") && kernel.watches.contains("vault") &&
              kernel.watches.contains("existing/nested"), "startup exclusions or directory inventory differ");
  const auto original = kernel.watches;
  kernel.calls.clear();
  fs::create_directory(project / "new");
  kernel.during_add = [&](std::string_view relative) {
    if (relative == "new") write(project / "new/during/deeper/file", "created while registering");
  };
  require(registry.extend("new", add), "new subtree registration failed");
  require(registry.size() == 7 && kernel.watches.contains("new/during/deeper"),
          "registration did not arm before enumerating late descendants");
  for (const auto &relative : kernel.calls)
    require(relative == "new" || relative.starts_with("new/"), "subtree registration revisited unrelated directories");
  for (const auto &[relative, descriptor] : original)
    require(registry.find(descriptor) && *registry.find(descriptor) == relative && kernel.watches[relative] == descriptor,
            "additive registration changed an existing descriptor mapping");
  kernel.during_add = {};
  kernel.calls.clear();
  require(registry.extend("new", add) && registry.size() == 7, "duplicate subtree event consumed directory quota");
  fs::create_directory(project / "sibling");
  require(registry.extend("sibling", add) && registry.size() == 8, "persistent directory budget differs");
  require(!registry.extend(".git/objects", add), "registration traversed a protected Git directory");
  require(!registry.extend("missing/child", add), "missing ancestor registered as an intact subtree");
  write(project / "regular", "not a directory");
  require(!registry.extend("regular/child", add), "file ancestor registered as a directory");
  for (const auto *path : {"", "../outside", "new/../sibling", "/absolute"})
    fails([&] { registry.extend(path, add); });
  fails([&] { registry.start(add); });
}
void private_store(const fs::path &project) {
  const auto storage = project / ".git/private/vault";
  { Store store(storage, {}, true); }
  write(project / ".git/unwatched/child/file", "Git private data");
  write(storage / "objects/unwatched/child/file", "Store private data");
  Kernel kernel{fs::canonical(project), {}, {}, {}};
  WatchDirectories registry(kernel.root, fs::canonical(storage), 2);
  require(registry.start([&](const fs::path &path) { return kernel.add(path); }), "direct private Store registration failed");
  require(registry.size() == 2 && kernel.watches.contains("") && kernel.watches.contains(".git/private/vault"),
          "direct Store registration enumerated excluded ancestor/storage directories");
}
void limits_and_aliases(const fs::path &project) {
  { Store store(project / "vault", {}, true); }
  Kernel kernel{fs::canonical(project), {}, {}, {}};
  WatchDirectories registry(kernel.root, fs::canonical(project / "vault"), 4);
  const auto add = [&](const fs::path &path) { return kernel.add(path); };
  require(registry.start(add), "bounded initial registration failed");
  fs::create_directory(project / "wide");
  for (unsigned i = 0; i != 128; ++i) fs::create_directory(project / "wide" / std::to_string(i));
  kernel.calls.clear();
  require(!registry.extend("wide", add) && registry.size() <= 4 && kernel.calls.size() <= 2,
          "registration quota was not enforced while queueing a wide subtree");
  registry.clear(); // The native caller also closes/discards its old instance.
  kernel.watches.clear();
  WatchDirectories aliases(kernel.root, fs::canonical(project / "vault"), 256);
  require(aliases.start(add), "alias fixture registration failed");
  const auto root_descriptor = kernel.watches.at("");
  fs::create_directory(project / "alias");
  require(!aliases.extend("alias", [&](const fs::path &) { return root_descriptor; }),
          "a reused descriptor silently relabeled another watched directory");
  require(aliases.find(root_descriptor) && aliases.find(root_descriptor)->empty(), "alias failure overwrote root mapping");
  const auto old = kernel.watches.at("wide");
  require(!aliases.extend("wide", [&](const fs::path &) { return 10000; }) &&
              aliases.find(old) && *aliases.find(old) == "wide",
          "replacement inode silently changed an existing path mapping");
  aliases.clear();
  require(aliases.size() == 0 && !aliases.find(root_descriptor), "registry reset kept an old generation mapping");
}
void cancellation(const fs::path &project) {
  { Store store(project / "vault", {}, true); }
  Kernel kernel{fs::canonical(project), {}, {}, {}};
  WatchDirectories registry(kernel.root, fs::canonical(project / "vault"), 16);
  const auto add = [&](const fs::path &path) { return kernel.add(path); };
  require(registry.start(add), "cancellation fixture registration failed");
  fs::create_directories(project / "new/child");
  std::stop_source stop;
  kernel.during_add = [&](std::string_view) { stop.request_stop(); };
  bool cancelled = false;
  try { registry.extend("new", add, stop.get_token()); } catch (const Cancelled &) { cancelled = true; }
  require(cancelled && !kernel.watches.contains("new/child"), "registration ignored cancellation after native arm");
  registry.clear();
  kernel.watches.clear();
  kernel.during_add = {};
  require(registry.start(add) && kernel.watches.contains("new/child"), "fresh registration did not recover a cancelled addition");
  fs::create_directory(project / "unavailable");
  require(!registry.extend("unavailable", [](const fs::path &) { return -1; }), "native registration failure was ignored");
}
} // namespace
int main() {
  const auto root = fs::temp_directory_path() / ("syrnck-watch-directories-" + hex(random_bytes(16)));
  struct Cleanup { fs::path root; ~Cleanup() { std::error_code error; fs::remove_all(root, error); } } cleanup{root};
  try {
    additions(root / "additions");
    private_store(root / "private-store");
    limits_and_aliases(root / "limits");
    cancellation(root / "cancellation");
    std::cout << "directory registration policy, bounds, aliases and cancellation passed\n";
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
