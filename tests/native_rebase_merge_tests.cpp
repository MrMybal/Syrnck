#include "native_history_fixture.hpp"
#include "native_rebase.hpp"
#include "native_graph.hpp"
#include "native_virtual_snapshots.hpp"
#include <iostream>
using namespace native_test;
namespace {
const Json &step(const Json &plan, const std::string &source) {
  for (const auto &row : plan["steps"])
    if (row["source"] == source)
      return row;
  throw Error("missing preserved rebase step");
}
void clean_graph(const fs::path &path, std::optional<Key> key, unsigned layout) {
  Fixture f(path, key);
  const auto zero = f.file("base"), a = f.file("a"), b = f.file("b"), custom = f.file("merge-only"),
             upstream = f.file("upstream");
  const auto base = f.revision(Json{{"asset", zero}});
  const auto left = f.revision(Json{{"asset", zero}, {"a", a}}, Json::array({base}));
  const auto right = f.revision(Json{{"asset", zero}, {"b", b}}, Json::array({base}));
  const auto merge = f.revision(Json{{"asset", zero}, {"a", a}, {"b", b}, {"resolution", custom}},
                                Json::array({left, right}));
  const auto tip = f.revision(Json{{"asset", zero}, {"a", a}, {"b", b}, {"resolution", custom}, {"tip", a}},
                              Json::array({merge}));
  const auto onto = f.revision(Json{{"asset", zero}, {"upstream", upstream}}, Json::array({base}));
  f.install(tip);
  f.pack(layout);
  const auto before = f.physical();
  NativeRebaseOptions opts;
  opts.preserve_merges = true;
  auto plan = prepare_native_rebase(f.store, path, "main", base, onto, opts, true, {});
  require(plan.report["ready"] == true && plan.report["steps"].size() == 4 &&
              plan.report["result_merges"] == 1 && plan.files.size() == 6 &&
              plan.files.contains("resolution") && plan.files.contains("upstream") &&
              !f.store.contains({Kind::revision, plan.target}) && f.physical() == before,
          "DAG rebase lost side-parent changes, manual merge edits or read-only planning");
  const auto &m = step(plan.report, merge), &l = step(plan.report, left), &r = step(plan.report, right);
  require(m["result_parents"] == Json::array({l["target"], r["target"]}) &&
              l["result_parents"] == Json::array({onto}) && r["result_parents"] == Json::array({onto}) &&
              step(plan.report, tip)["result_parents"] == Json::array({m["target"]}),
          "DAG rebase flattened or reversed actual parents");
  require(f.store.plan_rebase(path, "main", base, onto, opts)["target"] == plan.target,
          "DAG rebase IDs are not deterministic");
  auto todo = NativeRebaseTodo::parse(plan.report["todo"]);
  require(todo.steps[2].source == merge && todo.steps[2].action == "merge",
          "DAG todo did not retain merge actions");
  for (auto &row : todo.steps)
    if (row.source == left)
      row.action = "drop";
  opts.todo = todo;
  const auto dropped = prepare_native_rebase(f.store, path, "main", base, onto, opts, true, {});
  require(dropped.report["ready"] == true && !dropped.files.contains("a") && dropped.files.contains("b") &&
              dropped.files.contains("resolution") && dropped.files.contains("upstream"),
          "recreated merge reintroduced a deliberately dropped branch change");
  opts.todo.reset();
  opts.max_graph_work = 1;
  fails([&] { f.store.plan_rebase(path, "main", base, onto, opts); });
  opts = {};
  opts.preserve_merges = true;
  opts.max_delta_bytes = 1024;
  fails([&] { f.store.plan_rebase(path, "main", base, onto, opts); });
  opts = {};
  opts.preserve_merges = true;
  opts.graph_limits.max_revisions = 1;
  fails([&] { f.store.plan_rebase(path, "main", base, onto, opts); });
  opts = {};
  opts.preserve_merges = true;
  opts.max_commits = 3;
  fails([&] { f.store.plan_rebase(path, "main", base, onto, opts); });
  require(f.physical() == before, "failed DAG replay planning changed data");
  opts = {};
  opts.preserve_merges = true;
  const auto result = f.store.rebase_directory(path, "main", tip, base, onto, opts);
  require(result["target"] == plan.target && text(read_bytes(path / "resolution")) == "merge-only" &&
              text(read_bytes(path / "upstream")) == "upstream",
          "DAG rebase apply differs from preview");
  const auto actual_tip = f.store.metadata(Kind::revision, plan.target);
  const auto actual_merge = f.store.metadata(Kind::revision, actual_tip["parents"][0].get<std::string>());
  require(actual_merge["parents"] == m["result_parents"], "DAG candidate materialization lost merge parents");
  f.store.gc(true);
  require(f.store.fsck() > 0 && f.store.metadata(Kind::revision, tip)["parents"] == Json::array({merge}),
          "DAG rewrite archive did not retain original history");
}
void unchanged_resolutions(const fs::path &path) {
  Fixture f(path);
  const auto base_file = f.file("base"), a_file = f.file("left"), b_file = f.file("right"),
             manual = f.file("manual resolution");
  const auto base = f.revision(Json{{"asset", base_file}});
  const auto a = f.revision(Json{{"asset", a_file}}, Json::array({base}));
  const auto b = f.revision(Json{{"asset", b_file}}, Json::array({base}));
  const auto m = f.revision(Json{{"asset", manual}}, Json::array({a, b}));
  const auto onto = f.revision(Json{{"asset", base_file}, {"upstream", base_file}}, Json::array({base}));
  f.install(m);
  NativeRebaseOptions opts;
  opts.preserve_merges = true;
  auto plan = prepare_native_rebase(f.store, path, "main", base, onto, opts, true, {});
  require(plan.report["ready"] == true &&
              plan.files.at("asset").manifest == manual["manifest"].get<std::string>() &&
              step(plan.report, m)["recorded_resolutions_reused"] == Json::array({"asset"}),
          "unchanged merge inputs did not retain their recorded resolution");
  auto todo = NativeRebaseTodo::parse(plan.report["todo"]);
  for (auto &row : todo.steps)
    if (row.source == a)
      row.action = "drop";
  opts.todo = todo;
  auto blocked = f.store.plan_rebase(path, "main", base, onto, opts, true);
  require(blocked["ready"] == false && step(blocked, m)["conflicts"][0]["reason"] == "ambiguous_base",
          "changed old merge inputs silently reused a manual resolution");
  NativeRebaseResolutionSet choices{m, base, onto, {}};
  auto choice = NativeResolutionSet::parse(step(blocked, m)["resolution_context"]);
  choice.files.emplace("asset", NativeFileResolution{NativeResolutionChoice::incoming});
  choices.steps.emplace(m, choice);
  opts.resolutions = choices;
  const auto resolved = prepare_native_rebase(f.store, path, "main", base, onto, opts, true, {});
  require(resolved.report["ready"] == true &&
              resolved.files.at("asset").manifest == manual["manifest"].get<std::string>(),
          "explicit merge resolution was not applied after graph editing");
  opts.resolutions->steps.at(m).files.at("asset").choice = NativeResolutionChoice::base;
  fails([&] { f.store.plan_rebase(path, "main", base, onto, opts); });
}
void cousins_and_octopus(const fs::path &path) {
  Fixture f(path);
  const auto value = f.file("retained");
  const auto root = f.revision(Json{{"root", value}});
  const auto base = f.revision(Json{{"root", value}, {"base", value}}, Json::array({root}));
  const auto main = f.revision(Json{{"root", value}, {"base", value}, {"main", value}}, Json::array({base}));
  const auto cousin = f.revision(Json{{"root", value}, {"cousin", value}}, Json::array({root}));
  const auto foreign = f.revision(Json{{"foreign", value}});
  const auto merge = f.revision(
      Json{{"root", value}, {"base", value}, {"main", value}, {"cousin", value}, {"foreign", value}},
      Json::array({main, cousin, foreign}));
  const auto onto =
      f.revision(Json{{"root", value}, {"base", value}, {"upstream", value}}, Json::array({base}));
  f.install(merge);
  NativeRebaseOptions opts;
  opts.preserve_merges = true;
  auto original = prepare_native_rebase(f.store, path, "main", base, onto, opts, true, {});
  require(original.report["ready"] == true && original.files.size() == 6 &&
              step(original.report, cousin)["target"] == cousin &&
              step(original.report, foreign)["target"] == foreign &&
              step(original.report, merge)["result_parents"][1] == cousin &&
              step(original.report, merge)["result_parents"][2] == foreign,
          "default DAG rebase moved cousin branch points or unrelated roots");
  opts.rebase_cousins = true;
  auto rebased = prepare_native_rebase(f.store, path, "main", base, onto, opts, true, {});
  require(rebased.report["ready"] == true && rebased.files.size() == 6 &&
              step(rebased.report, cousin)["result_parents"] == Json::array({onto}) &&
              step(rebased.report, foreign)["result_parents"] == Json::array({onto}) &&
              step(rebased.report, merge)["result_parents"].size() == 3,
          "explicit cousin replay did not remap roots and preserve ordered octopus parents");
  const auto result = f.store.rebase_directory(path, "main", merge, base, onto, opts);
  const auto actual = f.store.metadata(Kind::revision, result["target"].get<std::string>());
  require(actual["parents"] == step(rebased.report, merge)["result_parents"],
          "octopus replay materialized different parent order");
  f.store.gc(true);
  require(f.store.fsck() > 0, "octopus/cousin graph lost closure");
}
void forced_parent_conflict(const fs::path &path) {
  Fixture f(path);
  const auto zero = f.file("base"), same = f.file("same"), different = f.file("different"),
             up = f.file("upstream");
  const auto base = f.revision(Json{{"asset", zero}});
  const auto a = f.revision(Json{{"asset", same}}, Json::array({base}));
  const auto b = f.revision(Json{{"asset", same}}, Json::array({base}));
  const auto merge = f.revision(Json{{"asset", same}}, Json::array({a, b}));
  const auto onto = f.revision(Json{{"asset", up}}, Json::array({base}));
  f.install(merge);
  NativeRebaseOptions opts;
  opts.preserve_merges = true;
  NativeRebaseResolutionSet choices{merge, base, onto, {}};
  auto blocked = f.store.plan_rebase(path, "main", base, onto, opts);
  auto side = NativeResolutionSet::parse(step(blocked, b)["resolution_context"]);
  side.files.emplace("asset", NativeFileResolution{NativeResolutionChoice::file,
                                                   different["manifest"].get<std::string>(), false});
  choices.steps.emplace(b, side);
  opts.resolutions = choices;
  blocked = f.store.plan_rebase(path, "main", base, onto, opts);
  auto first = NativeResolutionSet::parse(step(blocked, a)["resolution_context"]);
  first.files.emplace("asset", NativeFileResolution{NativeResolutionChoice::incoming});
  choices.steps.emplace(a, first);
  opts.resolutions = choices;
  blocked = f.store.plan_rebase(path, "main", base, onto, opts);
  require(blocked["ready"] == false && step(blocked, merge)["conflicts"][0]["reason"] == "merge_recreation" &&
              step(blocked, merge)["conflicts"][0]["current"] ==
                  step(blocked, merge)["conflicts"][0]["incoming"],
          "new parent conflict was hidden by a coincidentally equal provisional snapshot");
  auto resolve = NativeResolutionSet::parse(step(blocked, merge)["resolution_context"]);
  resolve.files.emplace("asset", NativeFileResolution{NativeResolutionChoice::incoming});
  choices.steps.emplace(merge, resolve);
  opts.resolutions = choices;
  auto plan = prepare_native_rebase(f.store, path, "main", base, onto, opts, true, {});
  require(plan.report["ready"] == true &&
              plan.files.at("asset").manifest == same["manifest"].get<std::string>(),
          "explicit provisional-parent conflict resolution failed");
  auto todo = NativeRebaseTodo::parse(plan.report["todo"]);
  for (auto &row : todo.steps)
    if (row.source == b) {
      row.action = "reword";
      row.message = "different side metadata";
    }
  opts.todo = todo;
  fails([&] { f.store.plan_rebase(path, "main", base, onto, opts); });
}
void interactive_folds(const fs::path &path) {
  Fixture f(path);
  const auto value = f.file("value");
  const auto base = f.revision(Json{{"base", value}});
  const auto a = f.revision(Json{{"base", value}, {"a", value}}, Json::array({base}));
  const auto a2 = f.revision(Json{{"base", value}, {"a", value}, {"a2", value}}, Json::array({a}));
  const auto b = f.revision(Json{{"base", value}, {"b", value}}, Json::array({base}));
  const auto m =
      f.revision(Json{{"base", value}, {"a", value}, {"a2", value}, {"b", value}}, Json::array({a2, b}));
  const auto onto = f.revision(Json{{"base", value}, {"upstream", value}}, Json::array({base}));
  f.install(m);
  NativeRebaseOptions opts;
  opts.preserve_merges = true;
  auto preview = f.store.plan_rebase(path, "main", base, onto, opts);
  auto todo = NativeRebaseTodo::parse(preview["todo"]);
  for (auto &row : todo.steps)
    if (row.source == a2)
      row.action = "squash";
  opts.todo = todo;
  auto plan = prepare_native_rebase(f.store, path, "main", base, onto, opts, true, {});
  require(plan.report["ready"] == true && plan.report["result_commits"] == 3 &&
              plan.report["result_merges"] == 1 &&
              step(plan.report, m)["result_parents"][0] == step(plan.report, a2)["target"],
          "graph-aware squash changed a side branch");
  for (auto &row : todo.steps)
    if (row.source == m)
      row.action = "fixup";
  opts.todo = todo;
  plan = prepare_native_rebase(f.store, path, "main", base, onto, opts, true, {});
  require(plan.report["ready"] == true && plan.report["result_commits"] == 2 &&
              plan.report["result_merges"] == 1,
          "explicit merge fold dropped the remaining side-parent edge");
  auto reversed = todo;
  std::reverse(reversed.steps.begin(), reversed.steps.end());
  opts.todo = reversed;
  fails([&] { f.store.plan_rebase(path, "main", base, onto, opts); });
  const auto fork = f.revision(Json{{"base", value}, {"a", value}, {"b", value}}, Json::array({a}));
  const auto merge_fork =
      f.revision(Json{{"base", value}, {"a", value}, {"a2", value}, {"b", value}}, Json::array({a2, fork}));
  f.install(merge_fork);
  opts = {};
  opts.preserve_merges = true;
  auto fork_todo = NativeRebaseTodo::parse(f.store.plan_rebase(path, "main", base, onto, opts)["todo"]);
  fork_todo.steps = {{a, "pick", {}}, {a2, "squash", {}}, {fork, "pick", {}}, {merge_fork, "merge", {}}};
  opts.todo = fork_todo;
  fails([&] { f.store.plan_rebase(path, "main", base, onto, opts); });
}
void signed_recovery(const fs::path &path, std::optional<Key> key, unsigned layout, bool abort) {
  std::string old, target, left, right;
  {
    Fixture f(path, key);
    const auto value = f.file("value");
    const auto base = f.revision(Json{{"base", value}});
    left = f.revision(Json{{"base", value}, {"left", value}}, Json::array({base}));
    right = f.revision(Json{{"base", value}, {"right", value}}, Json::array({base}));
    old = f.revision(Json{{"base", value}, {"left", value}, {"right", value}}, Json::array({left, right}));
    const auto onto = f.revision(Json{{"base", value}, {"upstream", value}}, Json::array({base}));
    f.install(old);
    f.pack(layout);
    Key seed;
    seed.fill(91);
    f.store.use_native_identity({"Graph author", "graph@example.invalid", 60}, SigningKey::from_seed(seed),
                                true);
    NativeRebaseOptions opts;
    opts.preserve_merges = true;
    opts.committer_timestamp = 1234;
    const auto before = f.physical();
    const auto preview = prepare_native_rebase(f.store, path, "main", base, onto, opts, true, {});
    target = preview.target;
    require(preview.report["ready"] == true && f.physical() == before, "signed DAG preview wrote state");
    std::stop_source stop;
    fails([&] {
      f.store.rebase_directory(path, "main", old, base, onto, opts,
                               {stop.get_token(), [&](const ProgressEvent &event) {
                                  if (event.phase == ProgressPhase::exporting && event.bytes > 0)
                                    stop.request_stop();
                                }});
    });
    require(f.store.checkout_status()["pending"] == true, "DAG rebase lost frozen recovery target");
    fails([&] { f.store.gc(true); });
  }
  Store store(path / ".syrnck", key);
  const auto recovered = store.recover_checkout(abort);
  require(recovered["state"] == (abort ? "aborted" : "committed") &&
              store.refs().at("heads/main").id == (abort ? old : target),
          "DAG checkout recovery changed the expected head");
  if (!abort) {
    const auto revision = store.metadata(Kind::revision, target);
    require(revision["parents"].size() == 2 && !revision["signature"].is_null() &&
                revision["committer"]["timestamp"] == 1234 && text(read_bytes(path / "upstream")) == "value",
            "signed DAG recovery lost parents, author clock or working bytes");
    for (const auto &parent : revision["parents"]) {
      const auto value = store.metadata(Kind::revision, parent.get<std::string>());
      validate_object(Kind::revision, bytes(value.dump()));
    }
  } else
    require(!fs::exists(path / "upstream"), "aborted DAG rebase kept incoming working bytes");
  store.gc(true);
  require(store.fsck() > 0 && store.metadata(Kind::revision, old)["parents"] == Json::array({left, right}),
          "DAG recovery lost source history or blocks");
}
void recursive_and_text(const fs::path &path) {
  Fixture f(path);
  const auto v = f.file("value");
  const auto base = f.revision(Json{{"base", v}});
  const auto a = f.revision(Json{{"base", v}, {"a", v}}, Json::array({base}));
  const auto b = f.revision(Json{{"base", v}, {"b", v}}, Json::array({base}));
  const auto ab = f.revision(Json{{"base", v}, {"a", v}, {"b", v}}, Json::array({a, b}));
  const auto ba = f.revision(Json{{"base", v}, {"a", v}, {"b", v}}, Json::array({b, a}));
  const auto head = f.revision(Json{{"base", v}, {"a", v}, {"b", v}, {"manual", v}}, Json::array({ab, ba}));
  const auto onto = f.revision(Json{{"base", v}, {"upstream", v}}, Json::array({base}));
  f.install(head);
  NativeRebaseOptions opts;
  opts.preserve_merges = true;
  const auto before = f.physical();
  auto plan = prepare_native_rebase(f.store, path, "main", base, onto, opts, true, {});
  const auto &root = step(plan.report, head);
  require(plan.report["ready"] == true &&
              root["recreated_parent_merge"]["stages"][0]["base_synthesis"]["virtual"] == true &&
              root["recreated_parent_merge"]["stages"][0]["merge_bases"].size() == 2 &&
              plan.files.size() == 5 && f.physical() == before,
          "replayed criss-cross graph did not synthesize its virtual common bases");
  f.store.rebase_directory(path, "main", head, base, onto, opts);
  const auto result = f.store.metadata(Kind::revision, plan.target);
  const auto ancestry = f.store.native_ancestry(result["parents"][0].get<std::string>(),
                                                result["parents"][1].get<std::string>());
  require(ancestry["merge_bases"].size() == 2, "materialized replay lost recursive common ancestry");
  std::string original;
  for (unsigned i = 0; i < 100; ++i)
    original += "int line_" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
  auto upstream = original;
  upstream.replace(upstream.find("= 10;"), 5, "= 99;");
  auto local = original;
  local.replace(local.find("= 70;"), 5, "= 98;");
  auto expected = upstream;
  expected.replace(expected.find("= 70;"), 5, "= 98;");
  const auto old = f.file(original), edited = f.file(local);
  const auto start = f.revision(Json{{"Source/old.cpp", old}});
  const auto rename = f.revision(Json{{"Source/new.cpp", old}}, Json::array({start}));
  const auto edit = f.revision(Json{{"Source/old.cpp", edited}}, Json::array({start}));
  const auto merged = f.revision(Json{{"Source/new.cpp", edited}}, Json::array({rename, edit}));
  const auto target = f.revision(Json{{"Source/old.cpp", f.file(upstream)}}, Json::array({start}));
  f.install(merged);
  opts.text_merge.emplace();
  opts.renames.emplace();
  const auto untouched = f.physical();
  plan = prepare_native_rebase(f.store, path, "main", start, target, opts, true, {});
  require(plan.report["ready"] == true && plan.files.size() == 1 && plan.files.contains("Source/new.cpp") &&
              plan.generated.size() == 1 && plan.generated.begin()->second == expected &&
              f.physical() == untouched,
          "DAG rename/text replay lost virtual content or wrote a preview");
  auto applied = f.store.rebase_directory(path, "main", merged, start, target, opts);
  require(applied["target"] == plan.target && text(read_bytes(path / "Source/new.cpp")) == expected &&
              !fs::exists(path / "Source/old.cpp"),
          "DAG rename/text materialization differs from prediction");
  f.store.gc(true);
  require(f.store.fsck() > 0, "DAG generated text was not retained");
}
void partial_and_scale(const fs::path &path) {
  Fixture f(path / "primary");
  const auto v = f.file("data");
  const auto base = f.revision(Json{{"Source/code", v}, {"Content/asset", v}});
  const auto a =
      f.revision(Json{{"Source/code", v}, {"Source/a", v}, {"Content/asset", v}}, Json::array({base}));
  const auto b =
      f.revision(Json{{"Source/code", v}, {"Source/b", v}, {"Content/asset", v}}, Json::array({base}));
  const auto head = f.revision(
      Json{{"Source/code", v}, {"Source/a", v}, {"Source/b", v}, {"Content/asset", v}}, Json::array({a, b}));
  const auto onto =
      f.revision(Json{{"Source/code", v}, {"Source/upstream", v}, {"Content/asset", v}}, Json::array({base}));
  f.install(head);
  const auto edit = path / "partial";
  f.store.create_native_branch("partial", head);
  f.store.register_partial_native_workspace(f.project, "edit", edit, {"Source"});
  f.store.use_native_workspace("edit");
  f.store.switch_directory(edit, "partial");
  NativeRebaseOptions opts;
  opts.preserve_merges = true;
  auto applied = f.store.rebase_directory(edit, "partial", head, base, onto, opts);
  const auto value = f.store.metadata(Kind::revision, applied["target"].get<std::string>());
  require(value["files"].contains("Content/asset") && !fs::exists(edit / "Content") &&
              fs::is_regular_file(edit / "Source/upstream") && f.store.refs().at("heads/main").id == head &&
              !fs::exists(f.project / "Source/upstream"),
          "graph rebase lost excluded assets or changed the primary workspace");
  f.store.use_native_workspace("");
  Json files = Json::object();
  for (unsigned i = 0; i < 500; ++i)
    files["Content/item-" + std::to_string(i)] = v;
  const auto root = f.revision(files);
  auto tip = root;
  for (unsigned i = 0; i < 32; ++i) {
    files["Source/change-" + std::to_string(i)] = v;
    tip = f.revision(files, Json::array({tip}));
  }
  auto upstream_files = f.store.metadata(Kind::revision, root)["files"];
  upstream_files["Source/upstream"] = v;
  const auto upstream = f.revision(upstream_files, Json::array({root}));
  f.install(tip);
  opts.max_delta_bytes = 32768;
  auto plan = f.store.plan_rebase(f.project, "main", root, upstream, opts, true);
  require(plan["ready"] == true && plan["virtual_snapshots"]["virtual_snapshots"] == 32 &&
              plan["virtual_snapshots"]["delta_entries"] == 32 &&
              plan["virtual_snapshots"]["delta_bytes"].get<std::size_t>() < 32768 &&
              plan["result_files"] == 533,
          "candidate cache retained whole asset snapshots per commit");
  const auto before = f.physical();
  std::stop_source stop;
  fails([&] {
    f.store.plan_rebase(f.project, "main", root, upstream, opts, false,
                        {stop.get_token(), [&](const ProgressEvent &event) {
                           if (event.complete && event.path.empty())
                             stop.request_stop();
                         }});
  });
  require(stop.stop_requested() && f.physical() == before, "cancelled DAG replay changed state");
}
void graph_and_deltas(const fs::path &path) {
  Fixture f(path);
  const auto file = f.file("base"), other = f.file("changed");
  const auto base = f.revision(Json{{"a", file}});
  const auto side = f.revision(Json{{"a", file}}, Json::array({base}));
  const auto head = f.revision(Json{{"a", file}}, Json::array({base, side}));
  NativeAncestryGraph graph(f.store, {head}, {20, 30}, {});
  const auto range = graph.range(base, head, 2, {});
  require(range == std::vector<std::string>{side, head}, "graph range excluded the wrong ancestors");
  fails([&] { graph.range(base, head, 1, {}); });
  auto virtual_id = hash(bytes("virtual node"));
  graph.append(virtual_id, {head, side}, {});
  require(graph.compare({side}, {virtual_id}, {}).source_is_ancestor &&
              graph.parents(virtual_id) == std::vector<std::string>{head, side},
          "appended virtual ancestry differs");
  graph.append(virtual_id, {head, side}, {});
  fails([&] { graph.append(virtual_id, {side, head}, {}); });
  fails([&] { graph.append(hash(bytes("bad")), {std::string(64, 'f')}, {}); });
  NativeVirtualSnapshots cache(f.store, 65536, {});
  auto first = read_native_snapshot(f.store, base, {});
  auto next = first;
  next.at("a") = read_native_snapshot(f.store, f.revision(Json{{"a", other}}), {}).at("a");
  cache.add(virtual_id, base, next);
  auto last = next;
  last.erase("a");
  last.emplace("b", first.at("a"));
  const auto second = hash(bytes("virtual second"));
  cache.add(second, virtual_id, last);
  require(cache.read(virtual_id).at("a").manifest == other["manifest"].get<std::string>() &&
              cache.read(second).contains("b") && !cache.read(second).contains("a") &&
              cache.report()["delta_entries"] == 3,
          "virtual branch delta reconstruction differs");
  fails([&] { cache.add(virtual_id, base, next); });
  fails([&] { cache.add(hash(bytes("missing")), std::string(64, 'f'), next); });
}
} // namespace
int main() {
  Temporary temp;
  try {
    for (bool encrypted : {false, true})
      for (unsigned layout = 0; layout < 3; ++layout) {
        std::optional<Key> key;
        if (encrypted) {
          key.emplace();
          key->fill(61);
        }
        clean_graph(temp.path / ("clean-" + std::to_string(encrypted) + "-" + std::to_string(layout)), key,
                    layout);
        for (bool abort : {false, true})
          signed_recovery(temp.path / ("recovery-" + std::to_string(encrypted) + "-" +
                                       std::to_string(layout) + "-" + std::to_string(abort)),
                          key, layout, abort);
      }
    unchanged_resolutions(temp.path / "resolutions");
    graph_and_deltas(temp.path / "graph");
    cousins_and_octopus(temp.path / "cousins");
    forced_parent_conflict(temp.path / "parent-conflict");
    interactive_folds(temp.path / "folds");
    recursive_and_text(temp.path / "recursive-text");
    partial_and_scale(temp.path / "partial-scale");
    std::cout << "Native graph-preserving rebase, recorded resolutions, virtual deltas and materialization "
                 "passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
