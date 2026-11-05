#include "native_history_fixture.hpp"
#include <iostream>
using namespace native_test;
namespace {
std::string frozen(Fixture &f, const Json &files, const std::optional<std::string> &base = {}) {
  const auto id =
      f.store
          .put(Kind::revision, bytes(Json{{"version", 1},
                                          {"parents", base ? Json::array({*base}) : Json::array()},
                                          {"message", "Syrnck staging v1: main"},
                                          {"timestamp", 0},
                                          {"files", files}}
                                         .dump()))
          .first.id;
  f.store.set_ref("staging/main", {Kind::revision, id});
  return id;
}
void planning(const fs::path &path, std::optional<Key> key, unsigned physical) {
  Fixture f(path, key);
  const auto a = f.file("baseline"), b = f.file("delete me"), extra = f.file("upstream");
  const auto base = f.revision(Json{{"a", a}, {"b", b}});
  f.install(base);
  f.write("a", "prepared");
  fs::remove(path / "b");
  const auto index = f.store.stage_directory(path, "main", {"."});
  const auto fresh = f.store.plan_rebase_stage("main");
  require(fresh["ready"] == true && fresh["changed"] == false && fresh["target"] == index,
          "fresh index rebase changed identity");
  const auto head = f.revision(Json{{"a", a}, {"b", b}, {"upstream", extra}}, Json::array({base}));
  f.store.set_ref("heads/main", {Kind::revision, head});
  f.write("a", "later dirty work");
  f.write("untracked", "leave this alone");
  f.pack(physical);
  const auto before = f.physical();
  const auto plan = f.store.plan_rebase_stage("main", true);
  require(plan["ready"] == true && plan["stale"] == true && plan["base"] == base && plan["head"] == head &&
              plan["index"] == index && plan["result_files"] == 2 && plan["changes"].size() == 2 &&
              plan["counts"]["removed"] == 1 && plan["counts"]["modified"] == 1 &&
              plan["objects_verified"] == true &&
              !f.store.contains({Kind::revision, plan["target"].get<std::string>()}) &&
              plan["target"] == f.store.plan_rebase_stage("main")["target"] && f.physical() == before,
          "stale index planning changed data, lost frozen deltas or produced unstable IDs");
  std::stop_source stop;
  fails([&] {
    f.store.plan_rebase_stage("main", true,
                              {stop.get_token(), [&](const ProgressEvent &) { stop.request_stop(); }});
  });
  fails([&] {
    f.store.plan_rebase_stage("main", false,
                              {{}, [](const ProgressEvent &) { throw Error("observer failed"); }});
  });
  require(f.physical() == before, "cancelled index rebase plan wrote data");
  const auto conflict = f.revision(Json{{"a", f.file("conflicting upstream")}, {"b", b}});
  f.store.set_ref("heads/main", {Kind::revision, conflict});
  const auto blocked = f.store.plan_rebase_stage("main");
  require(blocked["ready"] == false && blocked["target"].is_null() &&
              blocked["conflicts"][0]["reason"] == "content",
          "stale index conflict was silently overwritten");
  f.store.set_ref("heads/main", {Kind::revision, head});
  f.store.set_ref("archive/rebase-index/" + index, {Kind::revision, head});
  require(f.store.plan_rebase_stage("main")["blockers"][0]["reason"] == "archive_conflict",
          "index rebase adopted an unrelated archive");
  f.store.remove_ref("archive/rebase-index/" + index);
  f.store.reset_stage("main");
  fails([&] { f.store.plan_rebase_stage("main"); });
}
void application(const fs::path &path, std::optional<Key> key, unsigned physical) {
  Fixture f(path, key);
  const auto a = f.file("baseline"), b = f.file("removed"), extra = f.file("upstream");
  const auto base = f.revision(Json{{"a", a}, {"b", b}});
  f.install(base);
  f.write("a", "frozen selection");
  fs::remove(path / "b");
  const auto index = f.store.stage_directory(path, "main", {"."});
  const auto prepared = f.store.metadata(Kind::revision, index)["files"]["a"];
  const auto head = f.revision(Json{{"a", a}, {"b", b}, {"extra", extra}}, Json::array({base}));
  f.store.set_ref("heads/main", {Kind::revision, head});
  f.write("a", "later dirty bytes");
  f.write("untracked", "keep");
  f.pack(physical);
  const auto plan = f.store.plan_rebase_stage("main");
  const auto before = f.physical();
  const auto selection = f.store.native_head();
  fails([&] { f.store.rebase_stage("main", base, index); });
  fails([&] { f.store.rebase_stage("main", head, base); });
  require(f.physical() == before, "wrong expected staging rebase values wrote data");
  const auto refs = f.store.refs();
  std::stop_source stop;
  fails([&] {
    f.store.rebase_stage("main", head, index, {stop.get_token(), [&](const ProgressEvent &event) {
                                                 if (event.phase == ProgressPhase::publishing)
                                                   stop.request_stop();
                                               }});
  });
  require(f.store.refs() == refs && text(read_bytes(path / "a")) == "later dirty bytes" &&
              text(read_bytes(path / "untracked")) == "keep",
          "cancelled staging rebase moved refs or working files");
  fails([&] {
    f.store.rebase_stage("main", head, index, {{}, [](const ProgressEvent &event) {
                                                 if (event.phase == ProgressPhase::publishing)
                                                   throw Error("observer failed");
                                               }});
  });
  require(f.store.refs() == refs, "failed observer published a staging rebase");
  const auto result = f.store.rebase_stage("main", head, index);
  const auto target = result["target"].get<std::string>();
  const auto value = f.store.metadata(Kind::revision, target);
  require(result["state"] == "applied" && result["target"] == plan["target"] &&
              result["previous_index"] == index && value["parents"] == Json::array({head}) &&
              value["files"]["a"] == prepared && value["files"]["extra"] == extra &&
              !value["files"].contains("b") && f.store.refs().at("heads/main").id == head &&
              f.store.refs().at("archive/rebase-index/" + index).id == index &&
              f.store.native_head() == selection && text(read_bytes(path / "a")) == "later dirty bytes" &&
              !fs::exists(path / "b") && text(read_bytes(path / "untracked")) == "keep",
          "staging rebase lost its planned snapshot, archive or dirty workspace");
  const auto unchanged = f.physical();
  require(f.store.rebase_stage("main", head, target)["state"] == "unchanged" && f.physical() == unchanged,
          "fresh staging rebase wrote data");
  require(f.store.staged_status("main")["stale"] == false &&
              f.store.native_archives("rebase-index", {}, 100, true)["archives"][0]["revision"] == index,
          "staging rebase index is stale or its archive is not inspectable");
  const auto committed = f.store.commit_staged("main", "Publish rebased selection");
  const auto commit = f.store.metadata(Kind::revision, committed);
  require(commit["parents"] == Json::array({head}) && commit["files"] == value["files"],
          "staged commit after rebase lost frozen entries or current head");
  f.store.gc(true);
  (void)f.store.closure({{Kind::revision, index}, {Kind::revision, base}, {Kind::revision, committed}}, true);
  require(f.store.fsck() > 0, "staging rebase damaged retained data");
}
void resolutions(const fs::path &path, std::optional<Key> key, unsigned physical) {
  Fixture f(path, key);
  const auto original = f.file("baseline"), prepared = f.file("prepared"), current = f.file("upstream"),
             custom = f.file("manual selection", true), extra = f.file("keep upstream");
  const auto base = f.revision(Json{{"asset", original}});
  f.install(base);
  const auto index = frozen(f, Json{{"asset", prepared}}, base);
  const auto head = f.revision(Json{{"asset", current}, {"extra", extra}}, Json::array({base}));
  f.store.set_ref("heads/main", {Kind::revision, head});
  f.store.set_ref("pins/custom", {Kind::manifest, custom["manifest"].get<std::string>()});
  f.write("asset", "later work");
  f.pack(physical);
  const auto blocked = f.store.plan_rebase_stage("main");
  auto context = NativeResolutionSet::decode(bytes(blocked["resolution_context"].dump()));
  const auto before = f.physical();
  require(f.store.plan_rebase_stage_resolved("main", context)["ready"] == false,
          "empty choices hid an index conflict");
  fails([&] { f.store.rebase_stage_resolved("main", head, index, context); });
  auto stale = context;
  stale.head = base;
  fails([&] { f.store.plan_rebase_stage_resolved("main", stale); });
  stale = context;
  stale.source = head;
  fails([&] { f.store.plan_rebase_stage_resolved("main", stale); });
  stale = context;
  stale.base = head;
  fails([&] { f.store.plan_rebase_stage_resolved("main", stale); });
  stale = context;
  stale.parent = head;
  fails([&] { f.store.plan_rebase_stage_resolved("main", stale); });
  stale = context;
  stale.operation = "rebase";
  fails([&] { f.store.plan_rebase_stage_resolved("main", stale); });
  stale = context;
  stale.files["extra"] = {NativeResolutionChoice::remove};
  fails([&] { f.store.plan_rebase_stage_resolved("main", stale); });
  require(f.physical() == before, "invalid index resolution context wrote data");
  for (const auto choice :
       {NativeResolutionChoice::current, NativeResolutionChoice::incoming, NativeResolutionChoice::base,
        NativeResolutionChoice::remove, NativeResolutionChoice::file}) {
    context.files["asset"] = {choice};
    if (choice == NativeResolutionChoice::file)
      context.files["asset"] = {choice, custom["manifest"].get<std::string>(), true};
    const auto snapshot = f.physical();
    const auto plan = f.store.plan_rebase_stage_resolved("main", context, true);
    require(plan["ready"] == true && plan["resolved"].size() == 1 && f.physical() == snapshot,
            "resolved index plan was not ready or read-only");
    const auto result = f.store.rebase_stage_resolved("main", head, index, context);
    const auto target = result["target"].get<std::string>();
    const auto files = f.store.metadata(Kind::revision, target)["files"];
    require(result["target"] == plan["target"] && files["extra"] == extra &&
                f.store.refs().at("heads/main").id == head &&
                text(read_bytes(path / "asset")) == "later work",
            "resolved index publication changed upstream or working data");
    if (choice == NativeResolutionChoice::remove)
      require(!files.contains("asset"), "index removal resolution was ignored");
    else {
      const auto expected = choice == NativeResolutionChoice::current    ? current
                            : choice == NativeResolutionChoice::incoming ? prepared
                            : choice == NativeResolutionChoice::base     ? original
                                                                         : custom;
      require(files["asset"] == expected, "index resolution selected another file or mode");
    }
    fails([&] { f.store.plan_rebase_stage_resolved("main", context); });
    if (choice == NativeResolutionChoice::file)
      f.store.remove_ref("pins/custom");
    f.store.gc(true);
    (void)f.store.closure({{Kind::revision, target}, {Kind::revision, index}}, true);
    f.store.set_ref("staging/main", {Kind::revision, index});
  }
}
void pending_checkout(const fs::path &path) {
  Fixture f(path);
  const auto head = f.revision(Json{{"asset", f.file("source")}});
  const auto target = f.revision(Json{{"asset", f.file("checkout target")}}, Json::array({head}));
  f.install(head);
  f.store.set_ref("heads/feature", {Kind::revision, head});
  const auto index = f.store.stage_directory(path, "feature", {"asset"});
  std::stop_source stop;
  fails([&] {
    f.store.checkout_directory(path, "main", target,
                               {stop.get_token(), [&](const ProgressEvent &event) {
                                  if (event.phase == ProgressPhase::exporting && event.bytes)
                                    stop.request_stop();
                                }});
  });
  require(f.store.checkout_status()["pending"] == true, "checkout fixture did not publish its journal");
  const auto before = f.physical();
  const auto plan = f.store.plan_rebase_stage("feature");
  require(plan["ready"] == false && plan["blockers"][0]["reason"] == "pending_checkout",
          "index rebase did not block a pending checkout on another branch");
  fails([&] { f.store.rebase_stage("feature", head, index); });
  require(f.physical() == before, "blocked index rebase changed the active checkout");
  f.store.recover_checkout(true);
  require(f.store.refs().at("staging/feature").id == index && text(read_bytes(path / "asset")) == "source",
          "checkout abort lost the independent staging index");
}
void missing_custom(const fs::path &path) {
  std::string head, index, chunk;
  NativeResolutionSet choices;
  {
    Fixture f(path);
    const auto base = f.revision(Json{{"asset", f.file("base")}});
    f.install(base);
    index = frozen(f, Json{{"asset", f.file("prepared")}}, base);
    head = f.revision(Json{{"asset", f.file("upstream")}}, Json::array({base}));
    f.store.set_ref("heads/main", {Kind::revision, head});
    choices = NativeResolutionSet::parse(f.store.plan_rebase_stage("main")["resolution_context"]);
    const auto file = f.file("custom missing payload");
    const auto manifest = file["manifest"].get<std::string>();
    choices.files["asset"] = {NativeResolutionChoice::file, manifest, false};
    chunk = f.store.metadata(Kind::manifest, manifest)["blocks"][0]["id"].get<std::string>();
  }
  require(fs::remove(path / ".syrnck/objects/chunk" / chunk.substr(0, 2) / chunk),
          "missing custom fixture failed");
  Store store(path / ".syrnck");
  const auto refs = store.refs();
  const auto objects = store.objects();
  require(store.plan_rebase_stage_resolved("main", choices)["ready"] == true,
          "metadata index resolution read custom payloads");
  fails([&] { store.plan_rebase_stage_resolved("main", choices, true); });
  fails([&] { store.rebase_stage_resolved("main", head, index, choices); });
  require(store.refs() == refs && store.objects() == objects, "missing custom payload was published");
}
void namespace_choices(const fs::path &path) {
  Fixture f(path);
  const auto file = f.file("same bytes"), rep = f.file("same bytes", false, 2048);
  const auto base = f.revision(Json{{"asset", file}});
  f.install(base);
  const auto index = frozen(f, Json{{"asset", rep}, {"directory", f.file("prepared file")}}, base);
  const auto head =
      f.revision(Json{{"asset", file}, {"directory/child", f.file("upstream child")}}, Json::array({base}));
  f.store.set_ref("heads/main", {Kind::revision, head});
  const auto plan = f.store.plan_rebase_stage("main");
  auto choices = NativeResolutionSet::parse(plan["resolution_context"]);
  choices.files["directory/child"] = {NativeResolutionChoice::remove};
  const auto ready = f.store.plan_rebase_stage_resolved("main", choices, true);
  require(ready["ready"] == true && ready["resolved"].size() == 1,
          "index namespace choices did not recalculate collisions");
  const auto result = f.store.rebase_stage_resolved("main", head, index, choices);
  const auto value = f.store.metadata(Kind::revision, result["target"].get<std::string>());
  require(value["files"].contains("directory") && !value["files"].contains("directory/child"),
          "index file/directory conflict resolution was lost");
}
void missing_history(const fs::path &path) {
  std::string base, index, head, chunk;
  {
    Fixture f(path);
    const auto old = f.file("exclusive old history"), now = f.file("new content");
    base = f.revision(Json{{"asset", old}});
    f.install(base);
    index = frozen(f, Json{{"asset", now}}, base);
    head = f.revision(Json{{"asset", now}}, Json::array({base}));
    f.store.set_ref("heads/main", {Kind::revision, head});
    chunk = f.store.metadata(Kind::manifest, old["manifest"].get<std::string>())["blocks"][0]["id"]
                .get<std::string>();
  }
  require(fs::remove(path / ".syrnck/objects/chunk" / chunk.substr(0, 2) / chunk),
          "missing history fixture failed");
  Store store(path / ".syrnck");
  require(store.plan_rebase_stage("main")["ready"] == true, "metadata staging plan read historical payloads");
  const auto refs = store.refs();
  const auto objects = store.objects();
  fails([&] { store.plan_rebase_stage("main", true); });
  fails([&] { store.rebase_stage("main", head, index); });
  require(store.refs() == refs && store.objects() == objects, "staging rebase published damaged old history");
}
void representations_and_unborn(const fs::path &path) {
  Fixture f(path);
  const auto a = f.file(std::string(4096, 'x'), false, 1024),
             rep = f.file(std::string(4096, 'x'), false, 2048);
  require(a != rep, "representation fixture did not change chunk boundaries");
  const auto base = f.revision(Json{{"asset", a}});
  f.install(base);
  const auto index = frozen(f, Json{{"asset", rep}}, base);
  const auto head = f.revision(Json{{"asset", a}, {"extra", f.file("added")}}, Json::array({base}));
  f.store.set_ref("heads/main", {Kind::revision, head});
  const auto plan = f.store.plan_rebase_stage("main");
  require(plan["ready"] == true && plan["changes"].size() == 1 &&
              plan["changes"][0]["manifest_changed"] == true &&
              plan["changes"][0]["content_changed"] == false &&
              plan["changes"][0]["target"]["manifest"] == rep["manifest"] && plan["index"] == index,
          "index refresh lost an explicitly prepared representation");
  frozen(f, Json{{"new", f.file("unborn selection")}});
  require(f.store.plan_rebase_stage("main")["ready"] == true &&
              f.store.plan_rebase_stage("main")["base"].is_null(),
          "index from an unborn branch could not replay onto the first head");
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
          key->fill(53);
        }
        planning(temporary.path / (std::to_string(encrypted) + '-' + std::to_string(physical)), key,
                 physical);
        application(temporary.path / ("apply-" + std::to_string(encrypted) + '-' + std::to_string(physical)),
                    key, physical);
        resolutions(temporary.path /
                        ("resolve-" + std::to_string(encrypted) + '-' + std::to_string(physical)),
                    key, physical);
      }
    representations_and_unborn(temporary.path / "representation");
    missing_history(temporary.path / "missing");
    missing_custom(temporary.path / "missing-custom");
    pending_checkout(temporary.path / "pending");
    namespace_choices(temporary.path / "namespace");
    std::cout
        << "read-only staging rebase, frozen changes, conflicts, representations and cancellation passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
