#include "native_history_fixture.hpp"
#include <iostream>
using namespace native_test;
namespace {
std::function<void(std::string_view, const fs::path &)> hook;
struct Versions {
  std::string source, target;
  Versions(Fixture &f) {
    source = f.revision(Json{{"Alpha.txt", f.file("same bytes")},
                             {"Dir/Leaf.txt", f.file("renamed leaf")},
                             {"Flat", f.file("old flat")},
                             {"Tree/a", f.file("old tree a")},
                             {"Tree/nested/b", f.file("old tree b")},
                             {"keep", f.file("untouched")}});
    target = f.revision(Json{{"Flat/new", f.file("new nested")},
                             {"Tree", f.file("new flat", true)},
                             {"alpha.txt", f.file("same bytes")},
                             {"dir/leaf.txt", f.file("renamed leaf")},
                             {"keep", f.file("untouched")}},
                        Json::array({source}));
    f.install(source);
    f.store.set_ref("heads/target", {Kind::revision, target});
  }
};
std::map<std::string, std::string> working(const fs::path &root) {
  std::map<std::string, std::string> result;
  fs::recursive_directory_iterator entries(root), end;
  for (; entries != end; ++entries) {
    const auto relative = entries->path().lexically_relative(root);
    if (*relative.begin() == ".syrnck") {
      entries.disable_recursion_pending();
      continue;
    }
    if (entries->is_regular_file())
      result.emplace(utf8(relative), text(read_bytes(entries->path())));
  }
  return result;
}
void verify(Fixture &f, const std::string &revision) {
  std::map<std::string, std::string> expected;
  const auto value = f.store.metadata(Kind::revision, revision);
  for (const auto &[path, file] : value["files"].items()) {
    std::ostringstream output;
    f.store.reconstruct(file["manifest"].get<std::string>(), output);
    expected.emplace(path, output.str());
  }
  require(working(f.project) == expected, "namespace checkout lost exact names or bytes");
  require(f.store.refs().at("heads/main").id == revision && !f.store.checkout_status()["pending"].get<bool>(),
          "namespace checkout published an incomplete branch");
}
void application(const fs::path &root, std::optional<Key> key, unsigned storage) {
  Fixture f(root, key);
  const Versions versions(f);
  f.pack(storage);
  const auto before = f.physical();
  const auto plan = f.store.plan_checkout(root, "main", versions.target, true);
  require(plan["ready"] == true && plan["namespace_transition"] == true &&
              plan["remove_directories"] == Json::array({"Dir", "Tree", "Tree/nested"}) &&
              f.physical() == before,
          "namespace planning wrote data or lost directory removals");
  f.write("Tree/untracked.tmp", "preserve ignored work");
  const auto obstructed = f.physical();
  require(f.store.plan_checkout(root, "main", versions.target)["ready"] == false,
          "namespace transition adopted an untracked descendant");
  fails([&] { f.store.checkout_directory(root, "main", versions.target); });
  require(f.physical() == obstructed, "blocked namespace checkout moved data");
  fs::remove(root / "Tree/untracked.tmp");
  fs::create_directory(root / "Tree/empty-untracked");
  require(f.store.plan_checkout(root, "main", versions.target)["ready"] == false,
          "namespace transition removed an untracked empty directory");
  fs::remove(root / "Tree/empty-untracked");
  f.write("Dir/Leaf.txt", "dirty tracked text");
  require(f.store.plan_checkout(root, "main", versions.target)["ready"] == false,
          "namespace transition bypassed tracked hashes");
  f.write("Dir/Leaf.txt", "renamed leaf");
  const auto result = f.store.checkout_directory(root, "main", versions.target);
  require(result["version"] == 3 && result["namespace_transition"] == true &&
              f.store.refs().at(result["archive"].get<std::string>()).id == versions.source,
          "namespace checkout lost its versioned journal or source archive");
  verify(f, versions.target);
  f.store.gc(true);
  (void)f.store.closure({{Kind::revision, versions.source}}, true);
  f.store.checkout_directory(root, "main", versions.source);
  verify(f, versions.source);
  // Switch uses the same namespace executor and preserves both branch tips.
  const auto selected = f.store.switch_directory(root, "target");
  require(selected["version"] == 4 && f.store.native_head()["branch"] == "target" &&
              f.store.refs().at("heads/main").id == versions.source &&
              f.store.refs().at("heads/target").id == versions.target,
          "namespace branch switch moved a branch tip or lost HEAD");
  f.store.switch_directory(root, "main");
  verify(f, versions.source);
}
void interrupted(const fs::path &root, std::string_view checkpoint, bool abort) {
  Fixture f(root);
  const Versions versions(f);
  bool hit = false;
  hook = [&](std::string_view phase, const fs::path &target) {
    const bool selected =
        checkpoint == "backup"    ? phase == "move.published" && target.parent_path().filename() == "old"
        : checkpoint == "install" ? phase == "move.published" && target.parent_path() == fs::canonical(root)
                                  : phase == checkpoint;
    if (!hit && selected) {
      hit = true;
      throw Error("namespace interruption");
    }
  };
  fails([&] { f.store.checkout_directory(root, "main", versions.target); });
  hook = {};
  require(hit && f.store.checkout_status()["pending"] == true &&
              f.store.refs().at("heads/main").id == versions.source,
          "namespace interruption did not retain the old head and recovery journal");
  f.store.recover_checkout(abort);
  verify(f, abort ? versions.source : versions.target);
  f.store.gc(true);
  require(f.store.fsck() > 0, "namespace recovery damaged retained objects");
}
void interrupted_abort(const fs::path &root) {
  Fixture f(root);
  const Versions versions(f);
  hook = [](std::string_view phase, const fs::path &) {
    if (phase == "checkout.applied")
      throw Error("stop before publication");
  };
  fails([&] { f.store.checkout_directory(root, "main", versions.target); });
  hook = [](std::string_view phase, const fs::path &) {
    if (phase == "checkout.uninstalled")
      throw Error("stop after uninstall");
  };
  fails([&] { f.store.recover_checkout(true); });
  hook = {};
  require(f.store.checkout_status()["state"] == "restoring", "namespace rollback omitted its durable phase");
  f.store.recover_checkout();
  verify(f, versions.source);
}
void changed_work(const fs::path &root) {
  Fixture f(root);
  const Versions versions(f);
  hook = [](std::string_view phase, const fs::path &) {
    if (phase == "checkout.applied")
      throw Error("stop before publication");
  };
  fails([&] { f.store.checkout_directory(root, "main", versions.target); });
  hook = {};
  f.write("Tree", "user edit after installation");
  fails([&] { f.store.recover_checkout(); });
  fails([&] { f.store.recover_checkout(true); });
  require(text(read_bytes(root / "Tree")) == "user edit after installation" &&
              f.store.refs().at("heads/main").id == versions.source,
          "namespace recovery overwrote an observed working edit");
  f.write("Tree", "new flat");
  make_executable(root / "Tree");
  f.store.recover_checkout(true);
  verify(f, versions.source);
}
} // namespace
namespace syrnck::testing {
void io_checkpoint(std::string_view phase, const fs::path &path) {
  if (hook)
    hook(phase, path);
}
} // namespace syrnck::testing
int main() {
  Temporary temp;
  try {
    for (bool encrypted : {false, true})
      for (unsigned storage = 0; storage < 3; ++storage) {
        std::optional<Key> key;
        if (encrypted) {
          key.emplace();
          key->fill(43);
        }
        application(temp.path / (std::to_string(encrypted) + '-' + std::to_string(storage)), key, storage);
      }
    unsigned n = 0;
    for (const auto phase :
         {"checkout.prepared", "backup", "checkout.directory_removed", "checkout.evacuated",
          "checkout.directory_created", "install", "checkout.applied"})
      for (bool abort : {false, true})
        interrupted(temp.path / ("interrupted-" + std::to_string(++n)), phase, abort);
    interrupted_abort(temp.path / "abort-again");
    changed_work(temp.path / "changed");
    std::cout
        << "Recoverable file/directory and case transitions, exact names, switch, rollback and retention\n";
  } catch (const std::exception &error) {
    hook = {};
    std::cerr << error.what() << '\n';
    return 1;
  }
}
