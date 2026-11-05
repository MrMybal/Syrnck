#include "native_replay.hpp"
#include <iostream>
#include <source_location>
#include <sstream>
using namespace syrnck;
namespace {
template <class F> void fails(F function, std::source_location location = std::source_location::current()) {
  bool failed = false;
  try {
    function();
  } catch (const Error &) {
    failed = true;
  }
  require(failed, "expected native replay failure at line " + std::to_string(location.line()));
}
std::string manifest(const Store &store, std::string_view value, std::uint32_t chunk = 1024) {
  std::istringstream input{std::string(value)};
  return store.ingest(input, Chunking::fixed_size(chunk)).first;
}
Json file(const std::string &id, bool executable = false) {
  return Json{{"manifest", id}, {"executable", executable}};
}
std::string revision(const Store &store, Json files, Json parents = Json::array()) {
  std::map<std::string, Json> sorted;
  for (const auto &[path, entry] : files.items())
    sorted.emplace(path, entry);
  Json ordered = Json::object();
  for (const auto &[path, entry] : sorted)
    ordered[path] = entry;
  return store
      .put(Kind::revision, bytes(Json{{"version", 1},
                                      {"parents", parents},
                                      {"message", "replay fixture"},
                                      {"timestamp", 0},
                                      {"files", ordered}}
                                     .dump()))
      .first.id;
}
std::map<std::string, std::string> physical(const fs::path &root) {
  std::map<std::string, std::string> result;
  for (const auto &entry : fs::recursive_directory_iterator(root))
    if (entry.is_regular_file() && entry.path().filename() != "store.lock")
      result.emplace(utf8(entry.path().lexically_relative(root)), hash(read_bytes(entry.path())));
  return result;
}
void install(const Store &store, const fs::path &project, const std::string &id) {
  for (const auto &entry : fs::directory_iterator(project))
    if (entry.path().filename() != ".syrnck")
      fs::remove_all(entry.path());
  const auto snapshot = store.metadata(Kind::revision, id);
  for (const auto &[path, entry] : snapshot["files"].items()) {
    store.restore_file(entry["manifest"].get<std::string>(), project / path_from_utf8(path));
    if (entry["executable"].get<bool>())
      make_executable(project / path_from_utf8(path));
  }
  store.set_ref("heads/main", {Kind::revision, id});
}
bool conflict(const Json &plan, std::string_view path, std::string_view reason) {
  for (const auto &row : plan["conflicts"])
    if (row["path"].get<std::string>() == path && row["reason"].get<std::string>() == reason)
      return true;
  return false;
}
void scenario(const fs::path &project, std::optional<Key> key, unsigned layout) {
  Store store(project / ".syrnck", key, true);
  const auto a = manifest(store, "base"), b = manifest(store, "incoming"), ours = manifest(store, "ours");
  const auto script = manifest(store, "#!/bin/sh\nold\n"),
             edited_script = manifest(store, "#!/bin/sh\nnew\n");
  const auto same = manifest(store, "same logical bytes", 4),
             recoded = manifest(store, "same logical bytes", 7);
  const auto base = revision(store, Json{{"asset.bin", file(a)},
                                         {"ours.txt", file(a)},
                                         {"remove.txt", file(a)},
                                         {"script", file(script)},
                                         {"same", file(same)}});
  const auto current = revision(store,
                                Json{{"asset.bin", file(a)},
                                     {"ours.txt", file(ours)},
                                     {"remove.txt", file(a)},
                                     {"script", file(edited_script)},
                                     {"same", file(recoded)}},
                                Json::array({base}));
  const auto incoming = revision(store,
                                 Json{{"asset.bin", file(b)},
                                      {"ours.txt", file(a)},
                                      {"new.bin", file(b)},
                                      {"script", file(script, true)},
                                      {"same", file(same)}},
                                 Json::array({base}));
  install(store, project, current);
  store.set_ref("heads/feature", {Kind::revision, incoming});
  if (layout == 1)
    store.pack();
  if (layout == 2)
    store.pack({512 * 1024 * 1024, 3});
  const auto before = physical(project);
  auto planned =
      prepare_native_replay(store, project, "main", incoming, NativeReplayKind::cherry_pick, {}, true, {});
  const auto &files = planned.files;
  require(planned.report["ready"] == true && planned.report["objects_verified"] == true &&
              planned.report["head"] == current && planned.report["parent"] == base &&
              files.at("asset.bin").manifest == b && files.at("ours.txt").manifest == ours &&
              !files.contains("remove.txt") && files.at("new.bin").manifest == b &&
              files.at("script").manifest == edited_script && files.at("script").executable &&
              files.at("same").manifest == recoded && planned.report["changes"].size() == 4 &&
              planned.report["workspace"]["ready"] == true,
          "three-way replay lost independent content/mode changes or rechunked bytes");
  require(physical(project) == before, "replay planning wrote files or Store data");
  std::stop_source stop;
  fails([&] {
    store.plan_replay(project, "main", incoming, NativeReplayKind::cherry_pick, {}, false,
                      {stop.get_token(), [&](const ProgressEvent &) { stop.request_stop(); }});
  });
  fails([&] {
    store.plan_replay(project, "main", incoming, NativeReplayKind::cherry_pick, {}, false,
                      {{}, [](const ProgressEvent &) { throw Error("observer failure"); }});
  });
  require(physical(project) == before, "cancelled planning changed files");
  atomic_write(project / "asset.bin", bytes("dirty working edit"), true);
  auto blocked = store.plan_replay(project, "main", incoming, NativeReplayKind::cherry_pick);
  require(blocked["ready"] == false && blocked["workspace"]["ready"] == false,
          "dirty tracked workspace was accepted");
  install(store, project, current);
  atomic_write(project / "new.bin", bytes("untracked obstruction"), false);
  blocked = store.plan_replay(project, "main", incoming, NativeReplayKind::cherry_pick);
  require(blocked["ready"] == false, "untracked obstruction was accepted");
  fs::remove(project / "new.bin");
  store.stage_directory(project, "main", {"asset.bin"});
  require(store.plan_replay(project, "main", incoming, NativeReplayKind::cherry_pick)["ready"] == false,
          "replay accepted staging");
  store.reset_stage("main");
  store.set_ref("checkouts/active", store.put(Kind::chunk, bytes("pending fixture")).first);
  require(store.plan_replay(project, "main", incoming, NativeReplayKind::cherry_pick)["ready"] == false,
          "replay accepted a pending checkout");
  store.remove_ref("checkouts/active");
  // Revert removes only the selected commit's delta, preserving later changes.
  Json result_files = Json::object();
  for (const auto &[path, entry] : files)
    result_files[path] = file(entry.manifest, entry.executable);
  const auto applied = revision(store, result_files, Json::array({current}));
  install(store, project, applied);
  auto reverted =
      prepare_native_replay(store, project, "main", incoming, NativeReplayKind::revert, {}, false, {});
  require(reverted.report["ready"] == true && reverted.files.at("asset.bin").manifest == a &&
              reverted.files.at("ours.txt").manifest == ours && reverted.files.contains("remove.txt") &&
              !reverted.files.contains("new.bin") && reverted.files.at("script").manifest == edited_script &&
              !reverted.files.at("script").executable && reverted.files.at("same").manifest == recoded,
          "revert lost later unrelated content or modes");
  const auto empty = store.plan_replay(project, "main", incoming, NativeReplayKind::cherry_pick);
  require(empty["ready"] == false && empty["empty"] == true && empty["conflicts"].empty(),
          "already-applied changes did not produce an empty result");
  const auto merge = revision(store, result_files, Json::array({base, current}));
  fails([&] { store.plan_replay(project, "main", merge, NativeReplayKind::cherry_pick); });
  fails([&] { store.plan_replay(project, "main", merge, NativeReplayKind::revert, 0); });
  fails([&] { store.plan_replay(project, "main", merge, NativeReplayKind::revert, 3); });
  const auto mainline = store.plan_replay(project, "main", merge, NativeReplayKind::revert, 2);
  require(mainline["parent"] == current && mainline["mainline"] == 2 && mainline["ready"] == true,
          "explicit merge mainline was ignored");
  fails([&] { store.plan_replay(project, "main", base, NativeReplayKind::revert, 1); });
  fails([&] { store.plan_replay(project, "main", base, static_cast<NativeReplayKind>(77)); });
  // Root replay is a delta against the empty tree, in either direction.
  install(store, project, base);
  const auto root_revert = store.plan_replay(project, "main", base, NativeReplayKind::revert);
  require(root_revert["ready"] == true && root_revert["parent"].is_null() &&
              root_revert["mainline"].is_null() && root_revert["result_files"] == 0,
          "root revision revert differs");
  require(store.fsck() > 0, "planning damaged Store integrity");
}
void conflicts(const fs::path &project) {
  Store store(project / ".syrnck", {}, true);
  const auto a = manifest(store, "a"), b = manifest(store, "b"), c = manifest(store, "c");
  const auto base =
      revision(store, Json{{"content", file(a)}, {"delete-modify", file(a)}, {"modify-delete", file(a)}});
  const auto current =
      revision(store, Json{{"content", file(c)}, {"add-add", file(c)}, {"modify-delete", file(c)}},
               Json::array({base}));
  const auto incoming = revision(
      store, Json{{"content", file(b)}, {"add-add", file(b)}, {"delete-modify", file(b)}, {"safe", file(b)}},
      Json::array({base}));
  install(store, project, current);
  const auto before = physical(project);
  const auto plan = store.plan_replay(project, "main", incoming, NativeReplayKind::cherry_pick);
  require(!plan["ready"].get<bool>() && plan["workspace"].is_null() && plan["conflicts"].size() == 4 &&
              conflict(plan, "content", "content") && conflict(plan, "add-add", "add_add") &&
              conflict(plan, "delete-modify", "delete_modify") &&
              conflict(plan, "modify-delete", "modify_delete") && physical(project) == before,
          "file conflicts were silently resolved or wrote data");
  const auto empty = revision(store, Json::object());
  const auto case_current = revision(store, Json{{"Name", file(a)}}, Json::array({empty}));
  const auto case_incoming = revision(store, Json{{"name", file(b)}}, Json::array({empty}));
  store.set_ref("heads/main", {Kind::revision, case_current});
  require(conflict(store.plan_replay(project, "main", case_incoming, NativeReplayKind::cherry_pick), "name",
                   "case_collision"),
          "combined case collision was not reported");
  const auto prefix_current = revision(store, Json{{"dir", file(a)}}, Json::array({empty}));
  const auto prefix_incoming = revision(store, Json{{"dir/file", file(b)}}, Json::array({empty}));
  store.set_ref("heads/main", {Kind::revision, prefix_current});
  require(conflict(store.plan_replay(project, "main", prefix_incoming, NativeReplayKind::cherry_pick),
                   "dir/file", "file_directory_collision"),
          "combined prefix collision was not reported");
}
void missing_payload(const fs::path &project) {
  std::string incoming, chunk;
  {
    Store store(project / ".syrnck", {}, true);
    const auto a = manifest(store, "old"), b = manifest(store, "new");
    const auto base = revision(store, Json{{"file", file(a)}});
    incoming = revision(store, Json{{"file", file(b)}}, Json::array({base}));
    install(store, project, base);
    chunk = store.metadata(Kind::manifest, b)["blocks"][0]["id"].get<std::string>();
  }
  require(fs::remove(project / ".syrnck/objects/chunk" / chunk.substr(0, 2) / chunk),
          "missing chunk fixture failed");
  Store store(project / ".syrnck");
  require(store.plan_replay(project, "main", incoming, NativeReplayKind::cherry_pick)["ready"] == true,
          "metadata planning read incoming payloads");
  const auto before = physical(project);
  fails([&] { store.plan_replay(project, "main", incoming, NativeReplayKind::cherry_pick, {}, true); });
  require(physical(project) == before, "verified planning failure changed files");
}
} // namespace
int main() {
  const auto root = fs::temp_directory_path() / ("syrnck-native-replay-" + hex(random_bytes(16)));
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code error;
      fs::remove_all(path, error);
    }
  } cleanup{root};
  try {
    for (bool encrypted : {false, true})
      for (unsigned layout = 0; layout != 3; ++layout) {
        std::optional<Key> key;
        if (encrypted) {
          key.emplace();
          key->fill(53);
        }
        scenario(root / (std::to_string(encrypted) + '-' + std::to_string(layout)), key, layout);
      }
    conflicts(root / "conflicts");
    missing_payload(root / "missing");
    std::cout << "three-way replay planning, conflicts, mainline selection and workspace preflight passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
