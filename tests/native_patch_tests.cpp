#include "internal.hpp"
#include <iostream>
#include <sstream>
#include <source_location>
using namespace syrnck;
namespace {
const std::string source = "old\nkeep\n", target = "new\nkeep\nthird\n";
const std::string patch = "--- a/code.cpp\n+++ b/code.cpp\n@@ -1,2 +1,3 @@\n-old\n+new\n keep\n+third\n";
template <class F>
void fails(F fn, std::string_view expected = {},
           std::source_location where = std::source_location::current()) {
  try {
    fn();
  } catch (const Error &e) {
    require(expected.empty() || std::string_view(e.what()) == expected,
            "unexpected native patch failure at " + std::to_string(where.line()) + ": " + e.what());
    return;
  }
  throw Error("expected native patch failure at " + std::to_string(where.line()));
}
void write(const fs::path &path, std::string_view value) {
  fs::create_directories(path.parent_path());
  atomic_write(path, bytes(value), true);
}
std::string content(const Store &store, std::string_view revision, const std::string &path = "code.cpp") {
  const auto snapshot = store.metadata(Kind::revision, revision);
  std::ostringstream out;
  store.reconstruct(snapshot["files"][path]["manifest"].get<std::string>(), out);
  return out.str();
}
std::map<std::string, std::string> physical(const Store &store) {
  std::map<std::string, std::string> out;
  for (const auto &entry : fs::recursive_directory_iterator(store.root()))
    if (entry.is_regular_file() && entry.path().filename() != "store.lock")
      out.emplace(utf8(entry.path().lexically_relative(store.root())), hash(read_bytes(entry.path())));
  return out;
}
std::string id(const Json &plan) {
  return plan["plan_id"].get<std::string>();
}
bool blocked(const Json &plan, std::string_view reason) {
  for (const auto &row : plan["blockers"])
    if (row["reason"].get<std::string>() == reason)
      return true;
  return false;
}
void scenario(const fs::path &project, std::optional<Key> key, unsigned layout, bool indexed) {
  Store store(project / ".syrnck", key, true);
  write(project / "code.cpp", source);
  write(project / "other.txt", "original other");
  const auto first = store.commit_directory(project, "main", "baseline");
  auto snapshot = store.metadata(Kind::revision, first);
  snapshot["files"]["code.cpp"]["executable"] = true;
  const auto head = store.put(Kind::revision, bytes(snapshot.dump())).first;
  store.set_ref("heads/main", head);
  std::string previous;
  if (indexed) {
    write(project / "other.txt", "prepared other");
    previous = store.stage_directory(project, "main", {"other.txt"});
  }
  if (layout)
    store.pack(layout == 1 ? PackOptions{} : PackOptions{512 * 1024 * 1024, 2});
  fs::remove(project / "code.cpp");
  fs::create_directory(project / "code.cpp");
  write(project / ".syrnckignore", "code.cpp\n");
  const auto before = physical(store);
  const auto plan = store.plan_stage_patch("main", "code.cpp", bytes(patch));
  require(plan["ready"] == true && plan["operation"] == "stage-patch" &&
              plan["patch_hash"] == hash(bytes(patch)) &&
              plan["result"]["file_hash"] == hash(bytes(target)) && plan["result"]["hunks"] == 1 &&
              plan["result"]["additions"] == 2 && plan["result"]["deletions"] == 1 &&
              id(plan) == id(store.plan_stage_patch("main", "code.cpp", bytes(patch))) &&
              physical(store) == before,
          "native patch preview changed objects or identity");
  TextDiffOptions different;
  different.context = 0;
  fails([&] { store.stage_patch("main", "code.cpp", bytes(patch), id(plan), different); },
        "native text selection plan changed; refresh the plan");
  auto changed = patch;
  changed.replace(changed.find("+new\n"), 5, "+another\n");
  fails([&] { store.stage_patch("main", "code.cpp", bytes(changed), id(plan)); },
        "native text selection plan changed; refresh the plan");
  auto invalid = patch;
  invalid.replace(invalid.find("-old\n"), 5, "-wrong\n");
  fails([&] { store.stage_patch("main", "code.cpp", bytes(invalid), id(plan)); },
        "text patch context does not match source");
  require(before == physical(store), "invalid native patch wrote objects");
  for (auto phase : {ProgressPhase::ingesting, ProgressPhase::publishing}) {
    std::stop_source stop;
    const auto refs = store.refs();
    fails(
        [&] {
          store.stage_patch("main", "code.cpp", bytes(patch), id(plan), {},
                            {stop.get_token(), [&](const ProgressEvent &event) {
                               if (event.phase == phase &&
                                   (phase != ProgressPhase::publishing || event.path == "refs"))
                                 stop.request_stop();
                             }});
        },
        "operation cancelled");
    require(store.refs() == refs, "cancelled patch published references");
  }
  const auto refs = store.refs();
  fails(
      [&] {
        store.stage_patch("main", "code.cpp", bytes(patch), id(plan), {},
                          {{}, [](const ProgressEvent &event) {
                             if (event.phase == ProgressPhase::publishing && event.path == "refs")
                               throw Error("patch publish observer sentinel");
                           }});
      },
      "patch publish observer sentinel");
  require(store.refs() == refs, "failed observer published native patch");
  const auto result = store.stage_patch("main", "code.cpp", bytes(patch), id(plan));
  const auto index = result["index"].get<std::string>();
  const auto frozen = store.metadata(Kind::revision, index);
  require(content(store, index) == target &&
              content(store, index, "other.txt") == (indexed ? "prepared other" : "original other") &&
              frozen["files"]["code.cpp"]["executable"] == true &&
              frozen["parents"] == Json::array({head.id}) && store.refs().at("heads/main") == head &&
              fs::is_directory(project / "code.cpp"),
          "patch changed mode, neighbors, head or workspace");
  if (indexed)
    require(store.refs().at("archive/stage-index/" + previous).id == previous,
            "patch lost old index archive");
  store.gc(true);
  const auto commit = store.commit_staged("main", "external patch");
  store.gc(true);
  require(content(store, commit) == target && content(store, head.id) == source &&
              (!indexed || content(store, previous) == source),
          "patch history/index was lost after commit/GC");
}
void states(const fs::path &project) {
  Store store(project / ".syrnck", {}, true);
  write(project / "code.cpp", source);
  const auto first = store.commit_directory(project, "main", "base");
  const auto index = store.stage_directory(project, "main", {"code.cpp"});
  auto advanced = store.metadata(Kind::revision, first);
  advanced["parents"] = Json::array({first});
  advanced["message"] = "advanced";
  const auto head = store.put(Kind::revision, bytes(advanced.dump())).first;
  store.set_ref("heads/main", head);
  auto plan = store.plan_stage_patch("main", "code.cpp", bytes(patch));
  auto before = physical(store);
  require(blocked(plan, "stale_staging"), "stale index accepted for patch");
  fails([&] { store.stage_patch("main", "code.cpp", bytes(patch), id(plan)); });
  require(physical(store) == before, "stale patch wrote objects");
  store.set_ref("heads/main", {Kind::revision, first});
  store.set_ref("archive/stage-index/" + index, head);
  plan = store.plan_stage_patch("main", "code.cpp", bytes(patch));
  before = physical(store);
  require(blocked(plan, "archive_conflict"), "conflicting patch archive accepted");
  fails([&] { store.stage_patch("main", "code.cpp", bytes(patch), id(plan)); });
  require(physical(store) == before, "conflicting patch wrote objects");
  store.remove_ref("archive/stage-index/" + index);
  store.set_ref("archive/stage-index/" + index, {Kind::revision, index});
  const auto current = store.plan_stage_patch("main", "code.cpp", bytes(patch));
  // Exact existing archives can be reused, and working edits do not affect publication.
  const auto applied =
      store.stage_patch("main", "code.cpp", bytes(patch), id(current), {},
                        {{}, [&](const ProgressEvent &event) {
                           if (event.phase == ProgressPhase::publishing && event.path == "refs")
                             write(project / "code.cpp", "late working edit");
                         }});
  require(content(store, applied["index"].get<std::string>()) == target &&
              text(read_bytes(project / "code.cpp")) == "late working edit",
          "patch read working bytes");
  write(project / "code.cpp", source);
  store.stage_directory(project, "initial", {"code.cpp"});
  plan = store.plan_stage_patch("initial", "code.cpp", bytes(patch));
  const auto initial = store.stage_patch("initial", "code.cpp", bytes(patch), id(plan));
  require(store.metadata(Kind::revision, initial["index"].get<std::string>())["parents"].empty(),
          "initial patch acquired a parent");
  fails([&] { store.plan_stage_patch("missing", "code.cpp", bytes(patch)); },
        "native patch source file is absent");
  fails([&] { store.plan_stage_patch("main", "absent.cpp", bytes(patch)); },
        "native patch source file is absent");
  store.reset_stage("main");
  const std::string same = "--- a/code.cpp\n+++ b/code.cpp\n@@ -1 +1 @@\n-old\n+old\n";
  require(blocked(store.plan_stage_patch("main", "code.cpp", bytes(same)), "no_text_changes"),
          "no-op patch accepted");
  // Oversize metadata is refused before attempting to read a now-missing payload.
  const auto revision = store.metadata(Kind::revision, first);
  const auto manifest =
      store.metadata(Kind::manifest, revision["files"]["code.cpp"]["manifest"].get<std::string>());
  const auto chunk = manifest["blocks"][0]["id"].get<std::string>();
  fs::remove(store.root() / "objects/chunk" / chunk.substr(0, 2) / chunk);
  TextDiffOptions small;
  small.max_file_bytes = 1;
  fails([&] { store.plan_stage_patch("main", "code.cpp", bytes(patch), small); },
        "text patch source exceeds file budget");
  fails([&] { store.plan_stage_patch("main", "code.cpp", bytes(patch)); });
}
void incomplete(const fs::path &project) {
  Store store(project / ".syrnck", {}, true);
  write(project / "code.cpp", source);
  write(project / "other.txt", "unrelated data");
  const auto head = store.commit_directory(project, "main", "complete baseline");
  const auto snapshot = store.metadata(Kind::revision, head);
  const auto manifest =
      store.metadata(Kind::manifest, snapshot["files"]["other.txt"]["manifest"].get<std::string>());
  const auto chunk = manifest["blocks"][0]["id"].get<std::string>();
  fs::remove(store.root() / "objects/chunk" / chunk.substr(0, 2) / chunk);
  const auto plan = store.plan_stage_patch("main", "code.cpp", bytes(patch));
  require(plan["ready"] == true, "patch preview read unrelated payload");
  const auto refs = store.refs();
  fails([&] { store.stage_patch("main", "code.cpp", bytes(patch), id(plan)); });
  require(store.refs() == refs, "patch published incomplete closure");
}
} // namespace
int main() {
  const auto root = fs::temp_directory_path() / ("syrnck-patch-" + hex(random_bytes(16)));
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code ec;
      fs::remove_all(path, ec);
    }
  } cleanup{root};
  try {
    for (bool encrypted : {false, true})
      for (unsigned layout = 0; layout < 3; ++layout)
        for (bool indexed : {false, true}) {
          std::optional<Key> key;
          if (encrypted) {
            key.emplace();
            key->fill(71);
          }
          scenario(root / (std::to_string(encrypted) + '-' + std::to_string(layout) + '-' +
                           std::to_string(indexed)),
                   key, layout, indexed);
        }
    states(root / "states");
    incomplete(root / "incomplete");
    std::cout << "Verified exact native patches, frozen state, atomic archives, cancellation and GC\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
