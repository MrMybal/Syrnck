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
void missing_history(const fs::path &path) {
  std::string id, chunk;
  {
    Fixture f(path);
    const auto file = f.file("missing branch data");
    id = f.revision(Json{{"asset", file}});
    f.install(id);
    chunk = f.store.metadata(Kind::manifest, file["manifest"].get<std::string>())["blocks"][0]["id"]
                .get<std::string>();
  }
  require(fs::remove(path / ".syrnck/objects/chunk" / chunk.substr(0, 2) / chunk),
          "missing branch fixture failed");
  Store store(path / ".syrnck");
  require(store.native_branches()["branches"].size() == 1, "metadata branch inventory read payloads");
  const auto refs = store.refs();
  fails([&] { store.native_branches({}, 100, true); });
  fails([&] { store.create_native_branch("copy", id); });
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
      }
    missing_history(temporary.path / "missing");
    std::cout << "native branch pages, verified creation, frozen staging and cancellation passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
