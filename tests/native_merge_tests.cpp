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
  const auto ambiguous = f.store.plan_merge(path, "main", right);
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
      }
    std::cout << "native merge topology, bases, conflicts and read-only planning passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
