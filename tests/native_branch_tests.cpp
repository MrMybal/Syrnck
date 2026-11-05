#include "native_history_fixture.hpp"
#include <iostream>
using namespace native_test;
namespace {
void inventory(const fs::path &path, std::optional<Key> key, unsigned physical) {
  Fixture f(path, key);
  const auto empty = f.store.native_branches();
  require(empty["branches"].empty() && empty["head"]["branch"] == "main" &&
              empty["head"]["revision"].is_null(),
          "unborn branch inventory changed selection");
  const auto root = f.revision(Json{{"asset", f.file("retained")}});
  const auto head = f.revision(Json{{"asset", f.file("head")}}, Json::array({root}));
  f.install(head);
  const auto created = f.store.create_native_branch("feature/topic", root);
  require(created["state"] == "created" && created["current"] == false &&
              f.store.refs().at("heads/feature/topic").id == root,
          "native branch creation changed its target or selection");
  f.write("asset", "prepared");
  const auto index = f.store.stage_directory(path, "feature/topic", {"asset"});
  f.store.set_ref("heads/feature/topic", {Kind::revision, head});
  auto note = f.store.metadata(Kind::revision, root);
  note["message"] = std::string(255, 'x') + "\xc3\xa9" + std::string(300, 'y');
  const auto noted = f.store.put(Kind::revision, bytes(note.dump())).first;
  f.store.create_native_branch("z-long-note", noted.id);
  f.pack(physical);
  const auto before = f.physical();
  const auto page = f.store.native_branches({}, 1, true);
  require(page["branches"].size() == 1 && page["has_more"] == true &&
              page["branches"][0]["name"] == "feature/topic" && page["branches"][0]["index"] == index &&
              page["branches"][0]["index_base"] == root && page["branches"][0]["stale_index"] == true &&
              page["objects_verified"] == true,
          "branch page lost its staging state or cursor");
  const auto rest = f.store.native_branches(page["next_after"].get<std::string>(), 100, true);
  require(rest["branches"].size() == 2 && rest["branches"][0]["name"] == "main" &&
              rest["branches"][0]["current"] == true &&
              rest["branches"][1]["message"] == std::string(255, 'x') &&
              rest["branches"][1]["message_truncated"] == true && rest["has_more"] == false &&
              rest["next_after"].is_null() && f.physical() == before,
          "branch inventory wrote data, split UTF-8 or skipped a head");
  fails([&] { f.store.native_branches({}, 0); });
  fails([&] { f.store.native_branches({}, 1001); });
  fails([&] { f.store.native_branches("../escape"); });
  fails([&] { f.store.create_native_branch("main", root); });
  std::stop_source stop;
  fails([&] {
    f.store.native_branches({}, 100, true,
                            {stop.get_token(), [&](const ProgressEvent &) { stop.request_stop(); }});
  });
  fails([&] {
    f.store.create_native_branch("cancelled", root,
                                 {{}, [](const ProgressEvent &) { throw Error("observer failed"); }});
  });
  require(f.physical() == before, "failed branch operation changed data");
  f.store.remove_ref("heads/feature/topic");
  fails([&] { f.store.create_native_branch("feature/topic", root); });
  require(f.store.refs().at("staging/feature/topic").id == index, "branch creation replaced an orphan index");
  f.store.gc(true);
  require(f.store.native_branches({}, 100, true)["branches"].size() == 2 && f.store.fsck() > 0,
          "branch inventory damaged retained data");
}
void deletion(const fs::path &path, std::optional<Key> key, unsigned physical) {
  Fixture f(path, key);
  const auto root = f.revision(Json{{"asset", f.file("old branch bytes")}});
  const auto tip = f.revision(Json{{"asset", f.file("branch tip")}}, Json::array({root}));
  const auto main = f.revision(Json{{"asset", f.file("working base")}});
  f.install(main);
  f.store.create_native_branch("feature/topic", root);
  f.write("asset", "frozen selection");
  const auto index = f.store.stage_directory(path, "feature/topic", {"asset"});
  f.store.set_ref("heads/feature/topic", {Kind::revision, tip});
  f.write("asset", "later dirty bytes");
  f.write("untracked", "keep");
  f.pack(physical);
  const auto before = f.physical();
  const auto head = f.store.native_head();
  require(f.store.plan_delete_native_branch("feature/topic")["ready"] == false,
          "branch deletion silently discarded staging");
  const auto plan = f.store.plan_delete_native_branch("feature/topic", true, true);
  require(plan["ready"] == true && plan["branch"]["stale_index"] == true &&
              plan["archive_existing"] == false &&
              plan["archive"] == f.store.plan_delete_native_branch("feature/topic", true)["archive"] &&
              f.physical() == before,
          "branch deletion preview changed data or archive identity");
  require(f.store.plan_delete_native_branch("main")["blockers"][0]["reason"] == "selected_branch",
          "branch deletion accepted the selected branch");
  fails([&] { f.store.delete_native_branch("main", main); });
  fails([&] { f.store.delete_native_branch("feature/topic", root, index); });
  fails([&] { f.store.delete_native_branch("feature/topic", tip); });
  fails([&] { f.store.delete_native_branch("feature/topic", tip, root); });
  require(f.physical() == before, "blocked branch deletion wrote data");
  const auto refs = f.store.refs();
  std::stop_source stop;
  fails([&] {
    f.store.delete_native_branch("feature/topic", tip, index,
                                 {stop.get_token(), [&](const ProgressEvent &event) {
                                    if (event.phase == ProgressPhase::publishing)
                                      stop.request_stop();
                                  }});
  });
  require(f.store.refs() == refs, "cancelled deletion changed roots");
  const auto result = f.store.delete_native_branch("feature/topic", tip, index);
  const auto archive = result["archive"].get<std::string>();
  const auto prefix = "archive/branches/" + archive;
  require(result["state"] == "deleted" && result["archive"] == plan["archive"] &&
              !f.store.refs().contains("heads/feature/topic") &&
              !f.store.refs().contains("staging/feature/topic") &&
              f.store.refs().at(prefix + "/target").id == tip &&
              f.store.refs().at(prefix + "/index").id == index && f.store.native_head() == head &&
              text(read_bytes(path / "asset")) == "later dirty bytes" &&
              text(read_bytes(path / "untracked")) == "keep",
          "branch deletion lost its archive or changed the workspace");
  const auto row = f.store.native_branch_archive(archive, true)["archive"];
  require(row["name"] == "feature/topic" && row["revision"] == tip && row["index"] == index &&
              row["index_base"] == root && row["stale_index"] == true,
          "branch archive lost its original staging baseline");
  f.store.create_native_branch("empty", main);
  const auto other = f.store.delete_native_branch("empty", main)["archive"].get<std::string>();
  require(other != archive && f.store.native_branch_archive(other)["archive"]["index"].is_null(),
          "branch archive identity ignored the name or invented an index");
  const auto stable = f.physical();
  const auto first = f.store.native_branch_archives({}, 1, true);
  const auto last = f.store.native_branch_archives(first["next_after"].get<std::string>(), 1, true);
  require(first["has_more"] == true && last["has_more"] == false && last["next_after"].is_null() &&
              first["archives"].size() == 1 && last["archives"].size() == 1 &&
              first["archives"][0]["archive"] != last["archives"][0]["archive"] && f.physical() == stable,
          "branch archive pages repeated roots or wrote data");
  fails([&] { f.store.native_branch_archives({}, 0); });
  fails([&] { f.store.native_branch_archive(root); });
  f.store.gc(true);
  (void)f.store.closure({{Kind::revision, root}, {Kind::revision, tip}, {Kind::revision, index}}, true);
  require(f.store.native_branch_archive(archive, true)["archive"] == row && f.store.fsck() > 0,
          "GC lost deleted branch versions or its frozen index");
}
void archive_collision(const fs::path &path) {
  Fixture f(path);
  const auto id = f.revision(Json{{"asset", f.file("retained")}});
  f.install(id);
  f.store.create_native_branch("saved", id);
  const auto plan = f.store.plan_delete_native_branch("saved");
  const auto prefix = "archive/branches/" + plan["archive"].get<std::string>();
  f.store.set_ref(prefix + "/target", {Kind::revision, id}); // Incomplete owned archive.
  const auto before = f.physical();
  require(f.store.plan_delete_native_branch("saved")["blockers"][0]["reason"] == "archive_conflict",
          "branch deletion adopted partial archive roots");
  fails([&] { f.store.delete_native_branch("saved", id); });
  fails([&] { f.store.native_branch_archives(); });
  require(f.physical() == before, "conflicting archive changed branch data");
}
void missing_history(const fs::path &path) {
  std::string id, chunk;
  {
    Fixture f(path);
    const auto file = f.file("missing branch data");
    id = f.revision(Json{{"asset", file}});
    f.install(id);
    f.store.create_native_branch("saved", id);
    chunk = f.store.metadata(Kind::manifest, file["manifest"].get<std::string>())["blocks"][0]["id"]
                .get<std::string>();
  }
  require(fs::remove(path / ".syrnck/objects/chunk" / chunk.substr(0, 2) / chunk),
          "missing branch fixture failed");
  Store store(path / ".syrnck");
  require(store.native_branches()["branches"].size() == 2, "metadata branch inventory read payloads");
  const auto refs = store.refs();
  fails([&] { store.native_branches({}, 100, true); });
  fails([&] { store.create_native_branch("copy", id); });
  const auto objects = store.objects();
  require(store.plan_delete_native_branch("saved")["ready"] == true,
          "metadata branch deletion read payloads");
  fails([&] { store.delete_native_branch("saved", id); });
  require(store.objects() == objects, "branch deletion wrote its archive before data verification");
  require(store.refs() == refs, "branch creation published damaged history");
}
} // namespace
int main() {
  Temporary temporary;
  try {
    for (bool encrypted : {false, true})
      for (unsigned physical = 0; physical != 3; ++physical) {
        std::optional<Key> key;
        if (encrypted) {
          key.emplace();
          key->fill(54);
        }
        inventory(temporary.path / (std::to_string(encrypted) + '-' + std::to_string(physical)), key,
                  physical);
        deletion(temporary.path / ("delete-" + std::to_string(encrypted) + '-' + std::to_string(physical)),
                 key, physical);
      }
    missing_history(temporary.path / "missing");
    archive_collision(temporary.path / "collision");
    std::cout << "native branch pages, verified creation, frozen staging and cancellation passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
