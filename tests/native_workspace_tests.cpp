#include "native_workspace.hpp"
#include "fixture_cleanup.hpp"
#include <fstream>
#include <iostream>
#include <source_location>
#include <sstream>
#include <thread>
using namespace syrnck;
namespace {
template <class F> void fails(F function, std::source_location location = std::source_location::current()) {
  bool failed = false;
  try {
    function();
  } catch (const Error &) {
    failed = true;
  }
  require(failed, "expected workspace inspection failure at line " + std::to_string(location.line()));
}
void write(const fs::path &path, std::string_view value) {
  fs::create_directories(path.parent_path());
  atomic_write(path, bytes(value), true);
}
std::string manifest(const Store &store, std::string_view contents, Chunking chunking = {}) {
  std::istringstream input{std::string(contents)};
  return store.ingest(input, chunking).first;
}
std::string revision(const Store &store, Json value) {
  std::map<std::string, Json> sorted;
  for (const auto &[path, entry] : value["files"].items())
    sorted.emplace(path, entry);
  value["files"] = Json::object();
  auto &object = value["files"].get_ref<Json::object_t &>();
  object.reserve(sorted.size());
  for (const auto &[path, entry] : sorted)
    object.emplace_back(path, entry);
  return store.put(Kind::revision, bytes(value.dump())).first.id;
}
std::map<std::string, std::string> physical(const fs::path &root) {
  std::map<std::string, std::string> result;
  for (const auto &entry : fs::recursive_directory_iterator(root))
    if (entry.is_regular_file() && entry.path().filename() != "store.lock")
      result.emplace(utf8(entry.path().lexically_relative(root)), hash(read_bytes(entry.path())));
  return result;
}
bool conflict(const Json &plan, std::string_view path, std::string_view reason) {
  for (const auto &row : plan["conflicts"])
    if (row["path"].get<std::string>() == path && row["reason"].get<std::string>() == reason)
      return true;
  return false;
}
void layout(const Store &store, unsigned physical) {
  if (physical == 1)
    store.pack();
  if (physical == 2)
    store.pack({512 * 1024 * 1024, 3});
}
void planning(const fs::path &project, std::optional<Key> key, unsigned storage) {
  Store store(project / ".syrnck", key, true);
  const std::string original(1024 * 1024 + 3, 'a'), replacement(original.size(), 'b');
  write(project / ".syrnckignore", "Ignored/\n*.tmp\n");
  write(project / "Ignored/cache.tmp", "ignored data must still be protected");
  write(project / "edit.bin", original);
  write(project / "remove.txt", "remove");
  write(project / "same.txt", "same");
  write(project / "script", "#!/bin/sh\n");
  write(project / "rechunk.txt", "identical bytes with a different representation");
  const auto base = store.commit_directory(project, "main", "baseline");
  auto description = store.metadata(Kind::revision, base);
  description["parents"] = Json::array({base});
  description["message"] = "target";
  description["files"]["edit.bin"]["manifest"] = manifest(store, replacement);
  description["files"].erase("remove.txt");
  description["files"]["new/path.txt"] = Json{{"manifest", manifest(store, "new")}, {"executable", false}};
  description["files"]["script"]["executable"] = true;
  description["files"]["rechunk.txt"]["manifest"] =
      manifest(store, "identical bytes with a different representation", Chunking::fixed_size(1024));
  const auto target = revision(store, description);
  store.set_ref("heads/target", {Kind::revision, target});
  layout(store, storage);
  const auto before = physical(project);
  const auto plan = store.plan_checkout(project, "main", target);
  const auto expected_counts = Json{{"create", 1},         {"replace", 1}, {"remove", 1},
                                    {"set_executable", 1}, {"reuse", 1},   {"unchanged", 2}};
  // JSON object ordering is canonical for this report; compare fields directly.
  for (const auto &[name, count] : expected_counts.items())
    require(plan["counts"][name] == count, "checkout action counts differ");
  std::uint64_t hashed = 0;
  for (const auto &[path, file] : read_native_snapshot(store, base, {})) {
    (void)path;
    hashed += file.content.size;
  }
  require(plan["ready"] == true && plan["objects_verified"] == false && plan["source"] == base &&
              plan["target"] == target && plan["restore_bytes"] == replacement.size() + 3 &&
              plan["hashed_bytes"] == hashed && plan["create_directories"] == Json::array({"new"}) &&
              plan["conflicts"].empty() && plan["actions"].size() == 5,
          "clean checkout plan differs");
  const auto verified = store.plan_checkout(project, "main", target, true);
  require(verified["objects_verified"] == true && verified["ready"] == true &&
              verified["actions"] == plan["actions"],
          "verified checkout plan differs");
  require(physical(project) == before, "checkout planning changed files or Store data");
  const auto unchanged = store.plan_checkout(project, "main", base);
  require(unchanged["ready"] == true && unchanged["actions"].empty() && unchanged["counts"]["unchanged"] == 6,
          "identical checkout plan differs");

  const auto caller = std::this_thread::get_id();
  for (const auto phase : {ProgressPhase::scanning, ProgressPhase::planning, ProgressPhase::verifying}) {
    std::stop_source stop;
    bool seen = false;
    fails([&] {
      store.plan_checkout(
          project, "main", target, true,
          {stop.get_token(), [&](const ProgressEvent &event) {
             require(std::this_thread::get_id() == caller, "checkout observer changed threads");
             if (event.phase == phase && (phase != ProgressPhase::verifying || event.path.empty())) {
               seen = true;
               stop.request_stop();
             }
           }});
    });
    require(seen && physical(project) == before, "cancelled checkout planning changed files");
  }
  fails([&] {
    store.plan_checkout(project, "main", target, false, {{}, [](const ProgressEvent &event) {
                                                           if (event.phase == ProgressPhase::scanning)
                                                             throw Error("workspace observer sentinel");
                                                         }});
  });
  require(physical(project) == before, "failed checkout observer changed files");

  bool mutated = false;
  fails([&] {
    store.plan_checkout(project, "main", target, false,
                        {{}, [&](const ProgressEvent &event) {
                           if (!mutated && event.phase == ProgressPhase::verifying &&
                               event.path == "edit.bin") {
                             mutated = true;
                             std::ofstream output(project / "edit.bin", std::ios::binary | std::ios::app);
                             output.put('!');
                           }
                         }});
  });
  require(mutated, "workspace mutation fixture did not run");
  write(project / "edit.bin", original);

  const auto time = fs::last_write_time(project / "edit.bin");
  write(project / "edit.bin", replacement);
  fs::last_write_time(project / "edit.bin", time);
  const auto dirty = store.plan_checkout(project, "main", target);
  require(dirty["ready"] == false && conflict(dirty, "edit.bin", "tracked_modified"),
          "same-size/same-time edit was not protected");
  write(project / "edit.bin", original);
  write(project / "same.txt", "local edit to a file unchanged in the target");
  require(conflict(store.plan_checkout(project, "main", target), "same.txt", "tracked_modified"),
          "clean-tracked policy accepted an unrelated tracked edit");
  write(project / "same.txt", "same");
  fs::remove(project / "remove.txt");
  require(conflict(store.plan_checkout(project, "main", target), "remove.txt", "tracked_missing"),
          "local tracked deletion was not protected");
  write(project / "remove.txt", "remove");
  write(project / "new/path.txt", "new");
  require(conflict(store.plan_checkout(project, "main", target), "new/path.txt", "untracked_collision"),
          "identical untracked bytes were silently adopted");
  fs::remove(project / "new/path.txt");
  fixture_test::remove_empty_directory(project / "new");
  write(project / "new", "untracked ancestor");
  require(conflict(store.plan_checkout(project, "main", target), "new", "file_directory_transition"),
          "untracked ancestor was not protected");
  fs::remove(project / "new");
  fs::create_directories(project / "new/path.txt");
  require(conflict(store.plan_checkout(project, "main", target), "new/path.txt", "file_directory_transition"),
          "empty untracked directory was not protected");
  fixture_test::remove_empty_directory(project / "new/path.txt");
  fixture_test::remove_empty_directory(project / "new");

  auto ignored = description;
  ignored["files"]["Ignored/cache.tmp"] =
      Json{{"manifest", manifest(store, "ignored replacement")}, {"executable", false}};
  const auto ignored_target = revision(store, ignored);
  require(conflict(store.plan_checkout(project, "main", ignored_target), "Ignored/cache.tmp",
                   "untracked_collision"),
          "ignore rules concealed a checkout obstruction");
  write(project / ".syrnckignore", "Ignored/\nedit.bin\n");
  write(project / "edit.bin", replacement);
  require(conflict(store.plan_checkout(project, "main", target), "edit.bin", "tracked_modified"),
          "new ignore rule concealed a tracked edit");
  write(project / ".syrnckignore", "Ignored/\n*.tmp\n");
  write(project / "edit.bin", original);
  store.stage_directory(project, "main", {"edit.bin"});
  require(conflict(store.plan_checkout(project, "main", target), "", "staging_present"),
          "even a clean explicit staging index must be acknowledged");
  store.set_ref("heads/main", {Kind::revision, target});
  require(conflict(store.plan_checkout(project, "main", target), "", "stale_staging"),
          "stale staging did not block checkout");
  store.set_ref("heads/main", {Kind::revision, base});
  store.reset_stage("main");
  require(store.plan_checkout(project, "main", target)["ready"] == true,
          "checkout did not recover after conflicts");
}
void combined(const fs::path &project, std::optional<Key> key, unsigned storage) {
  Store store(project / ".syrnck", key, true);
  require(store.workspace_status(project, "main")["clean"] == true, "unborn workspace status differs");
  write(project / "file", "baseline");
  write(project / "removed", "preserve");
  const auto base = store.commit_directory(project, "main", "baseline");
  write(project / "file", "prepared");
  const auto staged = store.stage_directory(project, "main", {"file"});
  layout(store, storage);
  write(project / "file", "baseline"); // Clean against head, dirty against index.
  fs::remove(project / "removed");
  write(project / "untracked", "local");
  const auto before = physical(project);
  const auto status = store.workspace_status(project, "main");
  require(status["clean"] == false && status["staged"]["base"] == base &&
              status["staged"]["snapshot"] == staged && status["staged"]["counts"]["modified"] == 1 &&
              status["unstaged"]["base"] == staged && status["unstaged"]["counts"]["modified"] == 1 &&
              status["unstaged"]["counts"]["removed"] == 1 && status["unstaged"]["counts"]["untracked"] == 1,
          "combined status mixed head/index/workspace comparisons");
  require(physical(project) == before, "combined status wrote files");
  std::stop_source stop;
  stop.request_stop();
  fails([&] { store.workspace_status(project, "main", {stop.get_token()}); });
  fails([&] { store.plan_checkout(project, "main", base, false, {stop.get_token()}); });
  require(physical(project) == before, "pre-cancelled workspace inspection wrote files");
  store.reset_stage("main");
  write(project / "removed", "preserve");
  fs::remove(project / "untracked");
  store.stage_directory(project, "main", {"."});
  auto next = store.metadata(Kind::revision, base);
  next["message"] = "same files, moved branch";
  const auto advanced = revision(store, next);
  store.set_ref("heads/main", {Kind::revision, advanced});
  const auto stale = store.workspace_status(project, "main");
  require(stale["staged"]["clean"] == true && stale["unstaged"]["clean"] == true &&
              stale["staged"]["stale"] == true && stale["clean"] == false,
          "stale index alone must make combined status unclean");
}
void namespace_obstructions(const fs::path &project) {
  Store store(project / "vault", {}, true);
  write(project / "base.txt", "baseline");
  const auto base = store.commit_directory(project, "main", "baseline");
  const auto file = Json{{"manifest", manifest(store, "target")}, {"executable", false}};
  auto description = store.metadata(Kind::revision, base);
  description["files"]["vault/config.json"] = file;
  const auto protected_target = revision(store, description);
  const auto before = physical(project);
  require(conflict(store.plan_checkout(project, "main", protected_target), "vault", "protected_path"),
          "custom Store directory was not protected");
  require(physical(project) == before, "protected Store inspection changed files");
  description["files"].erase("vault/config.json");
  description["files"]["ALIAS.txt"] = file;
  const auto alias_target = revision(store, description);
  write(project / "alias.txt", "preserve untracked differently spelled file");
  require(conflict(store.plan_checkout(project, "main", alias_target), "ALIAS.txt", "path_spelling"),
          "portable spelling alias was not protected");
  if (!fs::exists(project / "ALIAS.txt")) {
    write(project / "ALIAS.txt", "second alias on a case-sensitive filesystem");
    require(conflict(store.plan_checkout(project, "main", alias_target), "ALIAS.txt", "portable_collision"),
            "multiple case-folded names were not protected");
  }
  description["files"].erase("ALIAS.txt");
  description["files"].erase("base.txt");
  description["files"]["base.txt/child"] = file;
  require(store.plan_checkout(project, "main", revision(store, description))["ready"] == true,
          "clean tracked file/directory conversion was blocked");
#ifndef _WIN32
  fs::permissions(project / "base.txt", fs::perms::owner_exec, fs::perm_options::add);
  require(conflict(store.plan_checkout(project, "main", base), "base.txt", "tracked_mode_changed"),
          "local executable-mode edit was not protected");
#endif
}
void partial(const fs::path &project) {
  Store origin(project / "origin", {}, true);
  const auto file = manifest(origin, "missing payload");
  Store store(project / "workspace/.syrnck", {}, true);
  const ObjectKey manifest_key{Kind::manifest, file};
  store.import(manifest_key, origin.get(manifest_key));
  Json description{{"version", 1},
                   {"parents", Json::array({std::string(64, '0')})},
                   {"message", "metadata only"},
                   {"timestamp", 0},
                   {"files", Json{{"file", Json{{"manifest", file}, {"executable", false}}}}}};
  const auto target = revision(store, description);
  const auto before = physical(store.root());
  const auto plan = store.plan_checkout(project / "workspace", "main", target);
  require(plan["ready"] == true && plan["objects_verified"] == false && plan["source"].is_null() &&
              plan["counts"]["create"] == 1 && plan["hashed_bytes"] == 0,
          "metadata-only checkout plan required payloads/history");
  fails([&] { store.plan_checkout(project / "workspace", "main", target, true); });
  require(physical(store.root()) == before, "failed verification changed Store data");
}
} // namespace
int main() {
  const auto root = fs::temp_directory_path() / ("syrnck-workspace-" + hex(random_bytes(16)));
  struct Cleanup {
    fs::path root;
    ~Cleanup() {
      std::error_code error;
      fs::remove_all(root, error);
    }
  } cleanup{root};
  try {
    for (bool encrypted : {false, true})
      for (unsigned storage = 0; storage != 3; ++storage) {
        std::optional<Key> key;
        if (encrypted) {
          key.emplace();
          key->fill(37);
        }
        const auto name = std::to_string(encrypted) + '-' + std::to_string(storage);
        planning(root / ("plan-" + name), key, storage);
        combined(root / ("combined-" + name), key, storage);
      }
    namespace_obstructions(root / "namespace");
    partial(root / "partial");
    std::cout << "native workspace status and checkout planning passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
