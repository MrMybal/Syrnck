#include "native_history_fixture.hpp"
#include <iostream>
using namespace native_test;
namespace {
void squash_range(const fs::path &path, std::optional<Key> key, unsigned physical, bool include_root) {
  Fixture f(path, key);
  const auto base = f.revision(Json{{"asset", f.file("first")}});
  const auto middle = f.revision(Json{{"asset", f.file("middle exclusive bytes")}}, Json::array({base}));
  const auto head = f.revision(Json{{"asset", f.file("tip", true)}}, Json::array({middle}));
  f.install(head);
  f.write("asset", "dirty working file");
  f.write("untracked", "keep");
  f.pack(physical);
  const auto count = include_root ? 3u : 2u;
  const auto before = f.physical();
  const auto plan = f.store.plan_squash("main", count, true);
  const auto expected_parents = include_root ? Json::array() : Json::array({base});
  require(plan["ready"] == true && plan["revisions"].size() == count && plan["head"] == head &&
              plan["parents"] == expected_parents && f.physical() == before,
          "squash plan changed data or selected wrong range");
  fails([&] { f.store.squash_commits("main", base, count, "wrong tip"); });
  std::stop_source stop;
  fails([&] {
    f.store.squash_commits("main", head, count, "cancelled",
                           {stop.get_token(), [&](const ProgressEvent &) { stop.request_stop(); }});
  });
  require(f.physical() == before, "early squash failure wrote data");
  const auto saved_head = f.store.native_head();
  const auto applied = f.store.squash_commits("main", head, count, "Combined work");
  const auto target = applied["target"].get<std::string>();
  const auto value = f.store.metadata(Kind::revision, target);
  require(value["parents"] == expected_parents && value["message"] == "Combined work" &&
              value["files"] == f.store.metadata(Kind::revision, head)["files"] &&
              f.store.refs().at("archive/squash/" + head).id == head &&
              f.store.native_head()["branch"] == saved_head["branch"] &&
              text(read_bytes(path / "asset")) == "dirty working file" &&
              text(read_bytes(path / "untracked")) == "keep",
          "squash lost tip representation, history or working bytes");
  f.store.gc(true);
  for (const auto &id : {base, middle, head, target})
    (void)f.store.closure({{Kind::revision, id}}, true);
  require(f.store.fsck() > 0, "squash damaged retained storage");
}
void guards(const fs::path &path) {
  Fixture f(path);
  const auto file = f.file("a");
  const auto root = f.revision(Json{{"asset", file}});
  const auto left = f.revision(Json{{"asset", file}}, Json::array({root}));
  const auto right = f.revision(Json{{"asset", file}}, Json::array({root}));
  const auto merge = f.revision(Json{{"asset", file}}, Json::array({left, right}));
  const auto tail = f.revision(Json{{"asset", file}}, Json::array({merge}));
  const auto head = f.revision(Json{{"asset", file}}, Json::array({tail}));
  f.install(head);
  require(f.store.plan_squash("main", 2)["ready"] == true &&
              f.store.plan_squash("main", 2)["parents"] == Json::array({merge}),
          "squash did not preserve a merge outside the range");
  require(f.store.plan_squash("main", 3)["blockers"][0]["reason"] == "merge_in_range",
          "squash silently dropped merge parents");
  fails([&] { f.store.squash_commits("main", head, 3, "drop merge"); });
  f.store.stage_directory(path, "main", {"asset"});
  require(f.store.plan_squash("main", 2)["ready"] == false, "squash accepted a clean index");
  f.store.reset_stage("main");
  f.store.set_ref("archive/squash/" + head, {Kind::revision, root});
  require(f.store.plan_squash("main", 2)["ready"] == false, "squash adopted another archive");
  f.store.remove_ref("archive/squash/" + head);
  f.install(left);
  const auto before = f.physical();
  require(f.store.plan_squash("main", 3)["blockers"][0]["reason"] == "count_exceeds_history",
          "squash accepted too many commits");
  for (auto count : {std::size_t{0}, std::size_t{1}, std::size_t{100001}, SIZE_MAX})
    fails([&] { f.store.plan_squash("main", count); });
  fails([&] {
    f.store.plan_squash("main", 2, false,
                        {{}, [](const ProgressEvent &) { throw Error("observer failure"); }});
  });
  require(f.physical() == before, "blocked squash wrote objects");
  std::stop_source stop;
  try {
    f.store.checkout_directory(path, "main", root,
                               {stop.get_token(), [&](const ProgressEvent &event) {
                                  if (event.phase == ProgressPhase::publishing && event.path == "checkout")
                                    stop.request_stop();
                                }});
  } catch (const Cancelled &) {
  }
  require(f.store.checkout_status()["pending"] == true && f.store.plan_squash("main", 2)["ready"] == false,
          "pending checkout did not block squash");
  f.store.recover_checkout(true);
}
void missing_ancestor_payload(const fs::path &path) {
  std::string head, chunk;
  fs::path block;
  {
    Fixture f(path);
    const auto old = f.file("exclusive old payload");
    const auto root = f.revision(Json{{"asset", old}});
    head = f.revision(Json{{"asset", f.file("tip")}}, Json::array({root}));
    f.install(head);
    chunk = f.store.metadata(Kind::manifest, old["manifest"].get<std::string>())["blocks"][0]["id"]
                .get<std::string>();
    block = path / ".syrnck/objects/chunk" / chunk.substr(0, 2) / chunk;
  }
  require(fs::remove(block), "missing squash fixture failed");
  Store store(path / ".syrnck");
  const auto refs = store.refs();
  const auto objects = store.objects();
  require(store.plan_squash("main", 2)["ready"] == true, "metadata squash plan audited block payloads");
  fails([&] { store.squash_commits("main", head, 2, "missing ancestor"); });
  require(store.refs() == refs && store.objects() == objects, "squash dropped damaged history");
}
} // namespace
int main() {
  Temporary temp;
  try {
    for (bool encrypted : {false, true})
      for (unsigned physical = 0; physical != 3; ++physical)
        for (bool root : {false, true}) {
          std::optional<Key> key;
          if (encrypted) {
            key.emplace();
            key->fill(48);
          }
          squash_range(temp.path / (std::to_string(encrypted) + '-' + std::to_string(physical) + '-' +
                                    std::to_string(root)),
                       key, physical, root);
        }
    guards(temp.path / "guards");
    missing_ancestor_payload(temp.path / "missing");
    std::cout << "linear/root squash, exact tip snapshots, merge guards, archives and GC passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
