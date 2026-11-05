#include "native_history_fixture.hpp"
#include <iostream>
using namespace native_test;
namespace {
void planning(const fs::path &path, std::optional<Key> key, unsigned physical) {
  Fixture f(path, key);
  const auto a = f.file("base"), b = f.file("changed"), own = f.file("ours"), extra = f.file("onto");
  const auto base = f.revision(Json{{"asset", a}});
  const auto first = f.revision(Json{{"asset", b}}, Json::array({base}));
  const auto head = f.revision(Json{{"asset", b}, {"own", own}}, Json::array({first}));
  const auto onto = f.revision(Json{{"asset", a}, {"extra", extra}}, Json::array({base}));
  f.install(head);
  f.store.set_ref("heads/upstream", {Kind::revision, onto});
  f.pack(physical);
  const auto before = f.physical();
  const auto plan = f.store.plan_rebase(path, "main", base, onto, {}, true);
  require(
      plan["ready"] == true && plan["mode"] == "rebase" && plan["commits"] == Json::array({first, head}) &&
          plan["steps"].size() == 2 && plan["completed_commits"] == 2 && plan["result_files"] == 3 &&
          plan["steps"][0]["head"] == onto && plan["steps"][1]["head"] == plan["steps"][0]["target"] &&
          plan["steps"][1]["target"] == plan["target"] &&
          !f.store.contains({Kind::revision, plan["target"].get<std::string>()}) && f.physical() == before,
      "rebase plan lost ordered deltas, wrote objects or produced stored virtual IDs");
  require(f.store.plan_rebase(path, "main", base, onto)["target"] == plan["target"],
          "rebase virtual IDs are not deterministic");
  const auto unchanged = f.store.plan_rebase(path, "main", base, base);
  require(unchanged["ready"] == true && unchanged["mode"] == "up_to_date" && unchanged["target"] == head,
          "same-base rebase changed identity");
  const auto move = f.store.plan_rebase(path, "main", head, onto);
  require(move["ready"] == true && move["mode"] == "move" && move["commits"].empty() &&
              move["target"] == onto,
          "empty-range rebase did not target onto");
  NativeRebaseOptions limited;
  limited.max_commits = 1;
  fails([&] { f.store.plan_rebase(path, "main", base, onto, limited); });
  limited.max_commits = 0;
  fails([&] { f.store.plan_rebase(path, "main", base, onto, limited); });
  std::stop_source stop;
  fails([&] {
    f.store.plan_rebase(path, "main", base, onto, {}, true,
                        {stop.get_token(), [&](const ProgressEvent &) { stop.request_stop(); }});
  });
  fails([&] {
    f.store.plan_rebase(path, "main", base, onto, {}, false,
                        {{}, [](const ProgressEvent &) { throw Error("observer failure"); }});
  });
  require(f.physical() == before, "failed rebase plan changed data");
  f.write("asset", "dirty");
  require(f.store.plan_rebase(path, "main", base, onto)["ready"] == false, "dirty rebase workspace accepted");
  f.install(head);
  f.store.stage_directory(path, "main", {"asset"});
  require(f.store.plan_rebase(path, "main", base, onto)["blockers"][0]["reason"] == "staging_exists",
          "rebase accepted staging");
  f.store.reset_stage("main");
  const auto conflict = f.revision(Json{{"asset", own}}, Json::array({base}));
  const auto blocked = f.store.plan_rebase(path, "main", base, conflict);
  require(blocked["ready"] == false && blocked["steps"].size() == 1 &&
              blocked["steps"][0]["conflicts"][0]["reason"] == "content" && blocked["target"].is_null() &&
              blocked["workspace"].is_null(),
          "rebase continued past a conflicting virtual commit");
  const auto foreign = f.revision(Json{{"foreign", own}});
  require(f.store.plan_rebase(path, "main", foreign, onto)["blockers"][0]["reason"] ==
              "base_not_on_linear_history",
          "rebase guessed another base");
  const auto merged = f.revision(Json{{"asset", b}}, Json::array({head, onto}));
  f.install(merged);
  require(f.store.plan_rebase(path, "main", base, onto)["blockers"][0]["reason"] == "merge_in_range",
          "rebase silently flattened a merge");
  const auto tail = f.revision(Json{{"asset", b}, {"own", own}}, Json::array({merged}));
  f.install(tail);
  require(f.store.plan_rebase(path, "main", merged, onto)["ready"] == true,
          "rebase refused a merge outside its range");
  require(f.store.fsck() > 0, "rebase planner damaged storage");
}
void missing_history(const fs::path &path) {
  std::string base, head, onto, chunk;
  {
    Fixture f(path);
    const auto old = f.file("exclusive historical bytes");
    base = f.revision(Json{{"asset", old}});
    head = f.revision(Json{{"asset", f.file("new")}}, Json::array({base}));
    onto = f.revision(Json{{"asset", old}, {"extra", f.file("extra")}}, Json::array({base}));
    f.install(head);
    chunk = f.store.metadata(Kind::manifest, old["manifest"].get<std::string>())["blocks"][0]["id"]
                .get<std::string>();
  }
  require(fs::remove(path / ".syrnck/objects/chunk" / chunk.substr(0, 2) / chunk),
          "missing rebase fixture failed");
  Store store(path / ".syrnck");
  require(store.plan_rebase(path, "main", base, onto)["ready"] == true,
          "metadata rebase plan audited payloads");
  fails([&] { store.plan_rebase(path, "main", base, onto, {}, true); });
}
} // namespace
int main() {
  Temporary temp;
  try {
    for (bool encrypted : {false, true})
      for (unsigned physical = 0; physical != 3; ++physical) {
        std::optional<Key> key;
        if (encrypted) {
          key.emplace();
          key->fill(50);
        }
        planning(temp.path / (std::to_string(encrypted) + '-' + std::to_string(physical)), key, physical);
      }
    missing_history(temp.path / "missing");
    std::cout
        << "read-only linear rebase, deterministic virtual chain, topology, conflicts and bounds passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
