#include "native_history_fixture.hpp"
#include "native_merge.hpp"
#include <iostream>
using namespace native_test;
namespace {
void planning(const fs::path &path, std::optional<Key> key, unsigned physical) {
  Fixture f(path, key);
  const auto a = f.file("original"), b = f.file("incoming"), own = f.file("our work");
  const auto base = f.revision(Json{{"asset", a}});
  const auto current = f.revision(Json{{"asset", a}, {"own", own}}, Json::array({base}));
  const auto source = f.revision(Json{{"asset", b}}, Json::array({base}));
  f.install(current);
  f.store.set_ref("heads/feature", {Kind::revision, source});
  f.pack(physical);
  const auto before = f.physical();
  const auto plan = prepare_native_merge(f.store, path, "main", source, {}, true, {});
  require(plan.report["ready"] == true && plan.report["mode"] == "merge" && plan.base == base &&
              plan.report["merge_bases"] == Json::array({base}) && plan.report["objects_verified"] == true &&
              plan.files.at("asset").manifest == b["manifest"].get<std::string>() &&
              plan.files.contains("own") && plan.report["workspace"]["ready"] == true &&
              f.physical() == before,
          "merge plan lost independent files or wrote data");
  std::stop_source stop;
  fails([&] {
    f.store.plan_merge(path, "main", source, {}, false,
                       {stop.get_token(), [&](const ProgressEvent &) { stop.request_stop(); }});
  });
  fails([&] {
    f.store.plan_merge(path, "main", source, {}, false,
                       {{}, [](const ProgressEvent &) { throw Error("observer failure"); }});
  });
  NativeMergeOptions limited;
  limited.limits.max_revisions = 1;
  fails([&] { f.store.plan_merge(path, "main", source, limited); });
  require(f.physical() == before, "failed merge planning changed data");
  f.write("asset", "dirty");
  require(f.store.plan_merge(path, "main", source)["ready"] == false, "dirty merge workspace was accepted");
  f.install(current);
  f.store.stage_directory(path, "main", {"asset"});
  require(f.store.plan_merge(path, "main", source)["ready"] == false, "merge accepted a prepared index");
  f.store.reset_stage("main");
  f.install(base);
  const auto forward = prepare_native_merge(f.store, path, "main", source, {}, false, {});
  require(forward.report["ready"] == true && forward.report["mode"] == "fast_forward" &&
              forward.files.at("asset").manifest == b["manifest"].get<std::string>(),
          "fast-forward plan differs");
  f.install(source);
  const auto unchanged = f.store.plan_merge(path, "main", base);
  require(unchanged["ready"] == true && unchanged["mode"] == "up_to_date" && unchanged["changes"].empty(),
          "already-contained source was not recognized");
  const auto conflict = f.revision(Json{{"asset", own}}, Json::array({base}));
  f.install(conflict);
  const auto blocked = f.store.plan_merge(path, "main", source);
  require(blocked["ready"] == false && blocked["conflicts"][0]["reason"] == "content" &&
              blocked["workspace"].is_null(),
          "concurrent binary conflict was hidden");
  const auto foreign = f.revision(Json{{"independent", own}});
  const auto unrelated = f.store.plan_merge(path, "main", foreign);
  require(unrelated["ready"] == false && unrelated["blockers"][0]["reason"] == "unrelated_histories",
          "unrelated histories were merged implicitly");
  NativeMergeOptions settings;
  settings.allow_unrelated_histories = true;
  require(f.store.plan_merge(path, "main", foreign, settings)["ready"] == true,
          "explicit unrelated-history merge was not planned");
  const auto arm_a = f.revision(Json{{"asset", a}}, Json::array({base}));
  const auto arm_b = f.revision(Json{{"asset", a}}, Json::array({base}));
  const auto left = f.revision(Json{{"asset", a}}, Json::array({arm_a, arm_b}));
  const auto right = f.revision(Json{{"asset", a}}, Json::array({arm_b, arm_a}));
  f.install(left);
  const auto recursive = f.store.plan_merge(path, "main", right);
  require(recursive["ready"] == true && recursive["base_virtual"] == true &&
              recursive["merge_bases"].size() == 2 && recursive["base_synthesis"]["ambiguous_paths"].empty(),
          "identical criss-cross bases were not synthesized");
  NativeMergeOptions strict;
  strict.recursive_bases = false;
  const auto ambiguous = f.store.plan_merge(path, "main", right, strict);
  require(ambiguous["ready"] == false && ambiguous["merge_bases"].size() == 2 &&
              ambiguous["blockers"][0]["reason"] == "multiple_merge_bases",
          "criss-cross base was chosen silently");
  settings = {};
  settings.base = arm_a;
  const auto explicit_base = f.store.plan_merge(path, "main", right, settings);
  require(explicit_base["ready"] == true && explicit_base["base"] == arm_a &&
              explicit_base["changes"].empty(),
          "explicit best base or topology-only merge was rejected");
  settings.base = base;
  fails([&] { f.store.plan_merge(path, "main", right, settings); });
  require(f.store.fsck() > 0, "merge planning damaged storage");
}
void application(const fs::path &path, std::optional<Key> key, unsigned physical) {
  std::string current, source, merged;
  {
    Fixture f(path, key);
    const auto a = f.file("original"), b = f.file("incoming"), own = f.file("our independent work");
    const auto base = f.revision(Json{{"asset", a}});
    current = f.revision(Json{{"asset", a}, {"own", own}}, Json::array({base}));
    source = f.revision(Json{{"asset", b}}, Json::array({base}));
    f.install(current);
    f.store.set_ref("heads/feature", {Kind::revision, source});
    f.pack(physical);
    const auto before = f.physical();
    require(f.store.merge_directory(path, "main", current, base, "noop")["state"] == "unchanged" &&
                f.physical() == before,
            "up-to-date merge wrote data");
    fails([&] { f.store.merge_directory(path, "main", base, source, "wrong tip"); });
    NativeMergeOptions only;
    only.fast_forward_only = true;
    require(f.store.plan_merge(path, "main", source, only)["blockers"][0]["reason"] ==
                "fast_forward_required",
            "fast-forward-only did not block divergence");
    fails([&] { f.store.merge_directory(path, "main", current, source, "ff only", only); });
    require(f.physical() == before, "rejected merge changed data");
    const auto result = f.store.merge_directory(path, "main", current, source, "merge branches");
    merged = result["target"].get<std::string>();
    require(result["state"] == "committed" && result["mode"] == "merge" &&
                f.store.metadata(Kind::revision, merged)["parents"] == Json::array({current, source}) &&
                text(read_bytes(path / "asset")) == "incoming" &&
                text(read_bytes(path / "own")) == "our independent work" &&
                f.store.refs().at("heads/feature").id == source,
            "merge did not retain both parent histories or independent files");
    for (const auto &[name, value] : f.store.refs()) {
      (void)value;
      if (name != "heads/main")
        f.store.remove_ref(name);
    }
    f.store.gc(true);
    require(f.store.metadata(Kind::revision, source)["parents"] == Json::array({base}) &&
                f.store.metadata(Kind::revision, current)["files"].contains("own"),
            "GC lost either merge parent");
    std::stop_source stop;
    fails([&] {
      f.store.replay_directory(path, "main", merged, merged, NativeReplayKind::revert, "undo merge", 1,
                               {stop.get_token(), [&](const ProgressEvent &event) {
                                  if (event.phase == ProgressPhase::exporting && event.bytes > 0)
                                    stop.request_stop();
                                }});
    });
    require(f.store.checkout_status()["pending"] == true, "merge revert did not leave a resumable journal");
  }
  {
    Store store(path / ".syrnck", key);
    const auto recovered = store.recover_checkout();
    require(recovered["state"] == "committed" && text(read_bytes(path / "asset")) == "original" &&
                text(read_bytes(path / "own")) == "our independent work",
            "merge revert recovery lost files");
    // A reverted merge remains an ancestor; explicitly return to its original
    // target for this independent abort scenario.
    store.checkout_directory(path, "main", current);
    std::stop_source stop;
    fails([&] {
      store.merge_directory(path, "main", current, source, "aborted merge", {},
                            {stop.get_token(), [&](const ProgressEvent &event) {
                               if (event.phase == ProgressPhase::exporting && event.bytes > 0)
                                 stop.request_stop();
                             }});
    });
    require(store.checkout_status()["pending"] == true && store.refs().at("heads/main").id == current,
            "interrupted merge published before completion");
  }
  Store store(path / ".syrnck", key);
  require(store.recover_checkout(true)["state"] == "aborted" && store.refs().at("heads/main").id == current &&
              text(read_bytes(path / "asset")) == "original" &&
              text(read_bytes(path / "own")) == "our independent work",
          "merge rollback changed committed files");
  store.gc(true);
  require(store.fsck() > 0, "merge rollback damaged storage");
}
void forward_modes(const fs::path &path) {
  Fixture f(path);
  const auto base_file = f.file("same content", false, 3), recoded = f.file("same content", false, 7);
  const auto base = f.revision(Json{{"asset", base_file}});
  const auto source = f.revision(Json{{"asset", recoded}}, Json::array({base}));
  f.install(base);
  NativeMergeOptions settings;
  settings.fast_forward_only = true;
  auto result = f.store.merge_directory(path, "main", base, source, "ignored ff message", settings);
  require(result["mode"] == "fast_forward" && result["target"] == source &&
              f.store.metadata(Kind::revision, source)["files"]["asset"] == recoded,
          "fast-forward failed to adopt exact manifest representation");
  f.install(base);
  settings = {};
  settings.no_fast_forward = true;
  result = f.store.merge_directory(path, "main", base, source, "explicit merge", settings);
  require(result["mode"] == "merge" && result["target"] != source &&
              f.store.metadata(Kind::revision, result["target"].get<std::string>())["parents"] ==
                  Json::array({base, source}),
          "no-ff failed to create a two-parent revision");
  settings.fast_forward_only = true;
  fails([&] { f.store.plan_merge(path, "main", source, settings); });
  const auto twin = f.revision(Json{{"asset", base_file}}, Json::array({base}));
  f.install(twin);
  const auto other = f.revision(Json{{"asset", base_file}}, Json::array({base}));
  result = f.store.merge_directory(path, "main", twin, other, "topology only");
  require(result["mode"] == "merge" &&
              f.store.metadata(Kind::revision, result["target"].get<std::string>())["parents"] ==
                  Json::array({twin, other}),
          "same-tree divergence was discarded");
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
          key->fill(59);
        }
        planning(temp.path / (std::to_string(encrypted) + '-' + std::to_string(physical)), key, physical);
        application(temp.path / ("apply-" + std::to_string(encrypted) + '-' + std::to_string(physical)), key,
                    physical);
      }
    forward_modes(temp.path / "forward");
    std::cout << "native merge topology, bases, conflicts and read-only planning passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
