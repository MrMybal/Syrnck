#include "native_history_fixture.hpp"
#include "native_merge_base.hpp"
#include <algorithm>
#include <iostream>
using namespace native_test;
namespace {
NativeResolutionSet resolution(const Json &report, std::string path, NativeResolutionChoice choice) {
  auto result = NativeResolutionSet::parse(report["resolution_context"]);
  result.files.emplace(std::move(path), NativeFileResolution{choice});
  return result;
}
void clean(const fs::path &path, std::optional<Key> key, unsigned physical) {
  Fixture f(path, key);
  const auto original = f.file("base"), aa = f.file("a"), bb = f.file("b"), cc = f.file("c");
  const auto root = f.revision(Json{{"asset", original}});
  const auto a = f.revision(Json{{"asset", original}, {"a", aa}}, Json::array({root}));
  const auto b = f.revision(Json{{"asset", original}, {"b", bb}}, Json::array({root}));
  const auto c = f.revision(Json{{"asset", original}, {"c", cc}}, Json::array({root}));
  const auto left =
      f.revision(Json{{"asset", original}, {"a", aa}, {"b", bb}, {"c", cc}}, Json::array({a, b, c}));
  const auto right =
      f.revision(Json{{"asset", original}, {"a", aa}, {"b", bb}, {"c", cc}}, Json::array({c, b, a}));
  f.install(left);
  f.pack(physical);
  const auto before = f.physical();
  auto plan = prepare_native_merge(f.store, path, "main", right, {}, true, {});
  require(plan.report["ready"] == true && plan.report["base_virtual"] == true && plan.base &&
              plan.report["merge_bases"].size() == 3 &&
              plan.report["base_synthesis"]["virtual_merges"] == 2 &&
              plan.report["base_synthesis"]["depth"] == 1 &&
              plan.report["base_synthesis"]["ambiguous_paths"].empty() && plan.files.size() == 4 &&
              plan.generated.empty() && plan.report["changes"].empty() &&
              !f.store.contains({Kind::revision, *plan.base}) && f.physical() == before,
          "clean three-base synthesis wrote objects or lost topology");
  f.install(right);
  const auto reversed = f.store.plan_merge(path, "main", left);
  require(reversed["base"] == plan.report["base"] &&
              reversed["base_synthesis"] == plan.report["base_synthesis"],
          "synthetic base depends on merge direction");
  f.install(left);
  const auto before_limits = f.physical();
  NativeMergeOptions limits;
  limits.base_limits.max_bases = 2;
  fails([&] { f.store.plan_merge(path, "main", right, limits); });
  limits = {};
  limits.base_limits.max_virtual_merges = 1;
  fails([&] { f.store.plan_merge(path, "main", right, limits); });
  limits = {};
  limits.base_limits.max_work = 1;
  fails([&] { f.store.plan_merge(path, "main", right, limits); });
  limits = {};
  limits.base_limits.max_input_bytes = 1024;
  fails([&] { f.store.plan_merge(path, "main", right, limits); });
  limits = {};
  limits.recursive_bases = false;
  require(f.store.plan_merge(path, "main", right, limits)["blockers"][0]["reason"] == "multiple_merge_bases",
          "strict base selection was ignored");
  limits.base = a;
  require(f.store.plan_merge(path, "main", right, limits)["ready"] == true,
          "explicit best base was not usable in strict mode");
  // Stop inside synthesis, after the initial graph query's completion event.
  std::stop_source stop;
  unsigned completed_graphs = 0;
  fails([&] {
    f.store.plan_merge(path, "main", right, {}, false,
                       {stop.get_token(), [&](const ProgressEvent &e) {
                          if (e.complete && e.path.empty() && ++completed_graphs == 2)
                            stop.request_stop();
                        }});
  });
  require(stop.stop_requested(), "recursive graph work did not expose cancellation");
  require(f.physical() == before_limits, "failed recursive planning changed Store or working files");
  const auto result = f.store.merge_directory(path, "main", left, right, "recursive topology");
  require(result["base_virtual"] == true && result["base"] == plan.report["base"] &&
              f.store.metadata(Kind::revision, result["target"].get<std::string>())["parents"] ==
                  Json::array({left, right}),
          "actual merge retained a synthetic parent");
  f.store.gc(true);
  require(f.store.fsck() > 0 && !f.store.contains({Kind::revision, *plan.base}),
          "synthetic nodes were materialized or GC lost history");
}
void uncertain(const fs::path &path, std::optional<Key> key, unsigned physical) {
  Fixture f(path, key);
  const auto zero = f.file("base"), one = f.file("ours"), two = f.file("theirs");
  const auto root = f.revision(Json{{"asset", zero}});
  const auto a = f.revision(Json{{"asset", one}}, Json::array({root}));
  const auto b = f.revision(Json{{"asset", two}}, Json::array({root}));
  const auto left = f.revision(Json{{"asset", one}}, Json::array({a, b}));
  const auto right = f.revision(Json{{"asset", two}}, Json::array({b, a}));
  const auto agreed = f.revision(Json{{"asset", one}}, Json::array({b, a}));
  f.install(left);
  f.pack(physical);
  const auto before = f.physical();
  NativeMergeOptions opts;
  opts.text_merge.emplace();
  opts.renames.emplace();
  auto plan = f.store.plan_merge(path, "main", right, opts, true);
  require(plan["ready"] == false && plan["conflicts"].size() == 1 &&
              plan["conflicts"][0]["reason"] == "ambiguous_base" &&
              plan["base_synthesis"]["ambiguous_paths"] == Json::array({"asset"}) &&
              plan["renames_suppressed"] == true && plan["renames"].is_null() && f.physical() == before,
          "conflicted virtual base silently decided divergent heads");
  auto same = f.store.plan_merge(path, "main", agreed, opts);
  require(same["ready"] == true && same["conflicts"].empty() && same["base_virtual"] == true,
          "agreeing actual heads were blocked by virtual uncertainty");
  opts.resolutions = resolution(plan, "asset", NativeResolutionChoice::base);
  fails([&] { f.store.plan_merge(path, "main", right, opts); });
  opts.resolutions = resolution(plan, "asset", NativeResolutionChoice::incoming);
  const auto resolved = prepare_native_merge(f.store, path, "main", right, opts, true, {});
  require(resolved.report["ready"] == true &&
              resolved.files.at("asset").manifest == two["manifest"].get<std::string>(),
          "explicit incoming resolution did not override virtual uncertainty");
  opts.resolutions->base = root;
  fails([&] { f.store.plan_merge(path, "main", right, opts); });
  opts.resolutions.reset();
  opts.conflict_copies = true;
  const auto copies = prepare_native_merge(f.store, path, "main", right, opts, true, {});
  require(copies.report["ready"] == true && copies.files.size() == 2 &&
              copies.report["resolved"][0]["reason"] == "ambiguous_base",
          "deliberate conflict copies did not preserve actual divergent versions");
  opts = {};
  opts.base = a;
  require(f.store.plan_merge(path, "main", right, opts)["ready"] == true,
          "deliberate actual best ancestor could not be selected");
  const auto removed = f.revision(Json::object(), Json::array({b, a}));
  opts = {};
  const auto missing = f.store.plan_merge(path, "main", removed, opts);
  require(missing["ready"] == false && missing["conflicts"][0]["reason"] == "ambiguous_base",
          "virtual uncertainty inferred a concurrent deletion");
  opts.resolutions = resolution(missing, "asset", NativeResolutionChoice::remove);
  require(prepare_native_merge(f.store, path, "main", removed, opts, true, {}).files.empty(),
          "explicit deletion of uncertain asset was rejected");
}
void text_and_recovery(const fs::path &path, std::optional<Key> key, unsigned physical, bool abort) {
  std::string left, right, combined_id, synthetic;
  {
    Fixture f(path, key);
    const auto zero = f.file("a\nb\nc\nd\ne\nf\n");
    const auto root = f.revision(Json{{"code", zero}});
    const auto a = f.revision(Json{{"code", f.file("A\nb\nc\nd\ne\nf\n")}}, Json::array({root}));
    const auto b = f.revision(Json{{"code", f.file("a\nb\nC\nd\ne\nf\n")}}, Json::array({root}));
    // Neither head equals the virtual base. The final text operation must read
    // its generated content without requiring an object-store write.
    left = f.revision(Json{{"code", f.file("A\nb\nC\nd\nE\nf\n")}}, Json::array({a, b}));
    right = f.revision(Json{{"code", f.file("A\nb\nC\nd\ne\nF\n")}}, Json::array({b, a}));
    f.install(left);
    f.pack(physical);
    Key seed;
    seed.fill(31);
    f.store.use_native_identity({"Virtual Merge", "virtual@example.invalid", 0}, SigningKey::from_seed(seed));
    const auto before = f.physical();
    NativeMergeOptions opts;
    opts.text_merge.emplace();
    opts.renames.emplace();
    const auto plan = prepare_native_merge(f.store, path, "main", right, opts, true, {});
    synthetic = *plan.base;
    require(plan.report["ready"] == true && plan.report["base_synthesis"]["ambiguous_paths"].empty() &&
                plan.generated.size() == 1 && plan.generated.begin()->second == "A\nb\nC\nd\nE\nF\n" &&
                f.physical() == before && !f.store.contains({Kind::revision, synthetic}),
            "virtual text base was not composed read-only or unused generated text leaked");
    combined_id = plan.files.at("code").manifest;
    std::stop_source stop;
    fails([&] {
      f.store.merge_directory(path, "main", left, right, "virtual text", opts,
                              {stop.get_token(), [&](const ProgressEvent &e) {
                                 if (e.phase == ProgressPhase::exporting && e.bytes > 0)
                                   stop.request_stop();
                               }});
    });
    require(f.store.checkout_status()["pending"] == true && f.store.refs().at("heads/main").id == left,
            "interrupted recursive merge lost frozen checkout");
    fails([&] { f.store.gc(true); });
  }
  Store store(path / ".syrnck", key);
  const auto result = store.recover_checkout(abort);
  require(result["state"] == (abort ? "aborted" : "committed") &&
              text(read_bytes(path / "code")) == (abort ? "A\nb\nC\nd\nE\nf\n" : "A\nb\nC\nd\nE\nF\n") &&
              !store.contains({Kind::revision, synthetic}),
          "recursive merge recovery changed final content or materialized virtual nodes");
  if (abort) {
    require(store.refs().at("heads/main").id == left, "recursive merge abort moved the branch");
  } else {
    const auto target = store.metadata(Kind::revision, result["target"].get<std::string>());
    require(target["version"] == 2 && !target["signature"].is_null() &&
                target["parents"] == Json::array({left, right}),
            "signed recursive recovery lost actual parents");
    validate_object(Kind::revision, bytes(target.dump()));
  }
  store.gc(true);
  require(store.fsck() > 0 && (abort || store.contains({Kind::manifest, combined_id})),
          "recursive text result did not survive GC");
}
void renamed_and_namespace(const fs::path &path) {
  Fixture f(path);
  const auto original = f.file("original"), changed = f.file("changed");
  const auto root = f.revision(Json{{"old", original}});
  const auto a = f.revision(Json{{"new", original}}, Json::array({root}));
  const auto b = f.revision(Json{{"old", changed}}, Json::array({root}));
  const auto left = f.revision(Json{{"new", changed}, {"ours", original}}, Json::array({a, b}));
  const auto right = f.revision(Json{{"new", changed}, {"theirs", original}}, Json::array({b, a}));
  f.install(left);
  NativeMergeOptions opts;
  opts.renames.emplace();
  auto plan = prepare_native_merge(f.store, path, "main", right, opts, true, {});
  require(plan.report["ready"] == true && plan.files.size() == 3 && !plan.files.contains("old") &&
              plan.report["base_synthesis"]["ambiguous_paths"].empty() &&
              !plan.report["base_synthesis"]["steps"][0]["renames"].is_null(),
          "virtual base failed to align an exact rename with old-path edits");
  for (bool prefix : {false, true}) {
    const std::string one = prefix ? "shape" : "Asset", two = prefix ? "shape/child" : "asset";
    const auto empty = f.revision(Json::object());
    const auto first = f.revision(Json{{one, original}}, Json::array({empty}));
    const auto second = f.revision(Json{{two, changed}}, Json::array({empty}));
    const auto ours = f.revision(Json{{one, original}}, Json::array({first, second}));
    const auto theirs = f.revision(Json{{two, changed}}, Json::array({second, first}));
    f.install(ours);
    opts = {};
    const auto conflict = f.store.plan_merge(path, "main", theirs, opts);
    require(conflict["ready"] == false && conflict["base_synthesis"]["ambiguous_paths"].size() == 2,
            "portable virtual namespace collision was hidden");
    opts.resolutions = resolution(conflict, one, NativeResolutionChoice::remove);
    opts.resolutions->files.emplace(two, NativeFileResolution{NativeResolutionChoice::incoming});
    plan = prepare_native_merge(f.store, path, "main", theirs, opts, true, {});
    require(plan.report["ready"] == true && plan.files.size() == 1 && plan.files.contains(two),
            "explicit virtual namespace resolution remained blocked");
    f.store.merge_directory(path, "main", ours, theirs, "resolve virtual namespace", opts);
    require(text(read_bytes(path / path_from_utf8(two))) == "changed",
            "virtual namespace transition lost content");
  }
}
void partial_signed(const fs::path &path) {
  Fixture f(path / "primary");
  const auto a_file = f.file("a"), b_file = f.file("b"), asset = f.file(std::string(4096, 'x'));
  const auto root = f.revision(Json{{"Source/code", a_file}, {"Content/asset", asset}});
  const auto a = f.revision(Json{{"Source/code", a_file}, {"Source/a", a_file}, {"Content/asset", asset}},
                            Json::array({root}));
  const auto b = f.revision(Json{{"Source/code", a_file}, {"Source/b", b_file}, {"Content/asset", asset}},
                            Json::array({root}));
  const auto left = f.revision(
      Json{{"Source/code", a_file}, {"Source/a", a_file}, {"Source/b", b_file}, {"Content/asset", asset}},
      Json::array({a, b}));
  const auto right = f.revision(
      Json{{"Source/code", b_file}, {"Source/a", a_file}, {"Source/b", b_file}, {"Content/asset", asset}},
      Json::array({b, a}));
  f.install(left);
  const auto edit = path / "edit";
  f.store.create_native_branch("partial", left);
  f.store.register_partial_native_workspace(f.project, "edit", edit, {"Source"});
  f.store.use_native_workspace("edit");
  f.store.switch_directory(edit, "partial");
  Key seed;
  seed.fill(29);
  f.store.use_native_identity({"Recursive Author", "recursive@example.invalid", 0},
                              SigningKey::from_seed(seed));
  const auto plan = f.store.plan_merge(edit, "partial", right, {}, true);
  require(plan["ready"] == true && plan["base_virtual"] == true,
          "partial linked workspace could not prepare recursive merge");
  const auto applied = f.store.merge_directory(edit, "partial", left, right, "signed recursive partial");
  const auto revision = f.store.metadata(Kind::revision, applied["target"].get<std::string>());
  validate_object(Kind::revision, bytes(revision.dump()));
  require(revision["version"] == 2 && !revision["signature"].is_null() &&
              revision["parents"] == Json::array({left, right}) &&
              revision["files"].contains("Content/asset") && !fs::exists(edit / "Content") &&
              text(read_bytes(edit / "Source/code")) == "b" && f.store.refs().at("heads/main").id == left &&
              text(read_bytes(f.project / "Source/code")) == "a",
          "signed partial recursive merge lost excluded logical assets or moved another workspace");
  f.store.gc(true);
  require(f.store.fsck() > 0, "partial recursive history failed retention");
}
void nested(const fs::path &path) {
  Fixture f(path);
  const auto root = f.revision(Json::object());
  const auto a = f.revision(Json::object(), Json::array({root}));
  const auto b = f.revision(Json::object(), Json::array({root}));
  const auto ab = f.revision(Json::object(), Json::array({a, b}));
  const auto ba = f.revision(Json::object(), Json::array({b, a}));
  const auto left = f.revision(Json::object(), Json::array({ab, ba}));
  const auto right = f.revision(Json::object(), Json::array({ba, ab}));
  f.install(left);
  auto plan = f.store.plan_merge(path, "main", right);
  require(plan["ready"] == true && plan["base_synthesis"]["depth"] == 2 &&
              plan["base_synthesis"]["virtual_merges"] == 2,
          "nested criss-cross ancestors were not recursively synthesized");
  NativeMergeOptions opts;
  opts.base_limits.max_depth = 1;
  fails([&] { f.store.plan_merge(path, "main", right, opts); });
  opts = {};
  opts.base_limits.max_virtual_merges = 1;
  fails([&] { f.store.plan_merge(path, "main", right, opts); });
  NativeMergeBaseLimits invalid;
  invalid.max_bases = 1;
  fails([&] { invalid.validate(); });
  invalid = {};
  invalid.max_virtual_merges = 0;
  fails([&] { invalid.validate(); });
  invalid = {};
  invalid.max_depth = 129;
  fails([&] { invalid.validate(); });
  invalid = {};
  invalid.max_work = 0;
  fails([&] { invalid.validate(); });
  invalid = {};
  invalid.max_input_bytes = 1023;
  fails([&] { invalid.validate(); });
}
} // namespace
int main() {
  Temporary temporary;
  try {
    for (bool encrypted : {false, true})
      for (unsigned physical = 0; physical < 3; ++physical) {
        std::optional<Key> key;
        if (encrypted) {
          key.emplace();
          key->fill(37);
        }
        const auto prefix = std::to_string(encrypted) + "-" + std::to_string(physical);
        clean(temporary.path / ("clean-" + prefix), key, physical);
        uncertain(temporary.path / ("uncertain-" + prefix), key, physical);
        for (bool abort : {false, true})
          text_and_recovery(temporary.path / ((abort ? "abort-text-" : "text-") + prefix), key, physical,
                            abort);
      }
    nested(temporary.path / "nested");
    renamed_and_namespace(temporary.path / "namespace");
    partial_signed(temporary.path / "partial");
    std::cout << "recursive native merge bases, ambiguity, budgets, text and frozen recovery passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
