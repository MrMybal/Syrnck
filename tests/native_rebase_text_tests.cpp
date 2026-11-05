#include "native_history_fixture.hpp"
#include "native_rebase.hpp"
#include "native_stage_rebase.hpp"
#include <iostream>
using namespace native_test;
namespace {
NativeRebaseOptions text_options() {
  NativeRebaseOptions value;
  value.text_merge.emplace();
  return value;
}
void linear(const fs::path &root, std::optional<Key> key, unsigned layout) {
  Fixture f(root, key);
  const auto base = f.revision(Json{{"code.cpp", f.file("a\r\nb\r\nc\r\nd")}});
  const auto first = f.revision(Json{{"code.cpp", f.file("a\r\nb\r\nc\r\nD")}}, Json::array({base}));
  const auto head = f.revision(Json{{"code.cpp", f.file("a\r\nb\r\nC\r\nD", true)}}, Json::array({first}));
  const auto onto = f.revision(Json{{"code.cpp", f.file("A\r\nb\r\nc\r\nd")}}, Json::array({base}));
  f.install(head);
  f.store.set_ref("heads/upstream", {Kind::revision, onto});
  f.pack(layout);
  auto settings = text_options();
  const auto before = f.physical();
  require(f.store.plan_rebase(root, "main", base, onto)["ready"] == false, "text rebase became implicit");
  const auto plan = prepare_native_rebase(f.store, root, "main", base, onto, settings, true, {});
  require(plan.report["ready"] == true && plan.report["completed_commits"] == 2 &&
              plan.report["text_merge"]["attempted_files"] == 2 && plan.generated.size() == 2 &&
              plan.generated.at(plan.files.at("code.cpp").manifest) == "A\r\nb\r\nC\r\nD" &&
              plan.files.at("code.cpp").executable,
          "virtual rebase lost an earlier text result or mode");
  std::vector<std::string> manifests;
  for (const auto &step : plan.report["steps"]) {
    require(step["text_merge"]["files"].size() == 1, "step duplicated earlier text rows");
    const auto manifest = step["text_merge"]["files"][0]["target"]["manifest"].get<std::string>();
    manifests.push_back(manifest);
    require(!f.store.contains({Kind::manifest, manifest}) &&
                !f.store.contains({Kind::revision, step["target"].get<std::string>()}),
            "rebase planning stored a virtual object");
  }
  auto limited = settings;
  limited.text_merge->max_work = 1;
  require(f.store.plan_rebase(root, "main", base, onto, limited)["ready"] == false,
          "rebase ignored text work limit");
  auto explicit_choice = settings;
  explicit_choice.resolutions = NativeRebaseResolutionSet::parse(plan.report["resolution_context"]);
  auto choice = NativeResolutionSet::parse(plan.report["steps"][0]["resolution_context"]);
  choice.files["code.cpp"] = {NativeResolutionChoice::current};
  explicit_choice.resolutions->steps[first] = choice;
  const auto chosen = f.store.plan_rebase(root, "main", base, onto, explicit_choice);
  require(chosen["ready"] == true && chosen["steps"][0]["empty"] == true &&
              chosen["steps"][0]["text_merge"]["attempted_files"] == 0 &&
              chosen["steps"][0]["resolved"][0]["choice"] == "current",
          "automatic rebase overrode an explicit choice or dropped an empty commit");
  explicit_choice.resolutions->steps[first].head = base;
  fails([&] { f.store.plan_rebase(root, "main", base, onto, explicit_choice); });
  fails([&] { f.store.rebase_directory(root, "main", first, base, onto, settings); });
  std::stop_source stop;
  fails([&] {
    f.store.rebase_directory(root, "main", head, base, onto, settings,
                             {stop.get_token(), [&](const ProgressEvent &event) {
                                if (event.phase == ProgressPhase::ingesting)
                                  stop.request_stop();
                              }});
  });
  require(f.physical() == before, "planning or cancellation before ingestion wrote data");
  f.write("code.cpp", "dirty bytes");
  const auto dirty = f.physical();
  fails([&] { f.store.rebase_directory(root, "main", head, base, onto, settings); });
  require(f.physical() == dirty, "blocked text rebase wrote candidates");
  f.install(head);
  const auto result = f.store.rebase_directory(root, "main", head, base, onto, settings);
  require(result["target"] == plan.target && result["replayed_commits"] == 2 &&
              text(read_bytes(root / "code.cpp")) == "A\r\nb\r\nC\r\nD",
          "text rebase applied another result");
  std::string parent = onto;
  for (std::size_t i = 0; i < 2; ++i) {
    const auto target = plan.report["steps"][i]["target"].get<std::string>();
    const auto revision = f.store.metadata(Kind::revision, target);
    const auto source = f.store.metadata(Kind::revision, i ? head : first);
    require(revision["parents"] == Json::array({parent}) && revision["message"] == source["message"] &&
                revision["timestamp"] == source["timestamp"] &&
                revision["files"]["code.cpp"]["manifest"] == manifests[i],
            "text rebase lost intermediate ancestry or source metadata");
    parent = target;
  }
  f.store.gc(true);
  for (const auto &manifest : manifests) {
    std::ostringstream output;
    f.store.reconstruct(manifest, output);
    require(output.str() == plan.generated.at(manifest), "GC lost intermediate merged text");
  }
  (void)f.store.closure({{Kind::revision, head}}, true);
}
void frozen(const fs::path &root, std::optional<Key> key, unsigned layout) {
  Fixture f(root, key);
  const auto base = f.revision(Json{{"code.cpp", f.file("a\nb\nc")}});
  f.install(base);
  f.write("code.cpp", "a\nb\nC");
  const auto index = f.store.stage_directory(root, "main", {"."});
  const auto head = f.revision(Json{{"code.cpp", f.file("A\nb\nc", true)}}, Json::array({base}));
  f.store.set_ref("heads/main", {Kind::revision, head});
  f.write("code.cpp", "later dirty work");
  f.write("untracked", "keep");
  f.pack(layout);
  NativeStageRebaseOptions settings;
  settings.text_merge.emplace();
  const auto before = f.physical();
  const auto selected = f.store.native_head();
  require(f.store.plan_rebase_stage("main")["ready"] == false &&
              f.store.plan_rebase_stage_with_options("main", {}) == f.store.plan_rebase_stage("main"),
          "staging text merge became implicit or changed legacy plans");
  const auto plan = f.store.plan_rebase_stage_with_options("main", settings, true);
  const auto manifest = plan["text_merge"]["files"][0]["target"]["manifest"].get<std::string>();
  require(plan["ready"] == true && plan["text_merge"]["attempted_files"] == 1 &&
              !f.store.contains({Kind::manifest, manifest}),
          "staging plan wrote merged text");
  auto explicit_choice = settings;
  explicit_choice.resolutions = NativeResolutionSet::parse(plan["resolution_context"]);
  explicit_choice.resolutions->files["code.cpp"] = {NativeResolutionChoice::current};
  const auto chosen = f.store.plan_rebase_stage_with_options("main", explicit_choice);
  require(chosen["ready"] == true && chosen["empty"] == true && chosen["text_merge"]["attempted_files"] == 0,
          "staging ignored explicit choices");
  explicit_choice.resolutions->head = base;
  fails([&] { f.store.plan_rebase_stage_with_options("main", explicit_choice); });
  auto limited = settings;
  limited.text_merge->max_file_bytes = 1;
  require(f.store.plan_rebase_stage_with_options("main", limited)["ready"] == false,
          "staging ignored text limits");
  fails([&] { f.store.rebase_stage_with_options("main", base, index, settings); });
  fails([&] { f.store.rebase_stage_with_options("main", head, base, settings); });
  std::stop_source stop;
  fails([&] {
    f.store.rebase_stage_with_options("main", head, index, settings,
                                      {stop.get_token(), [&](const ProgressEvent &event) {
                                         if (event.phase == ProgressPhase::ingesting)
                                           stop.request_stop();
                                       }});
  });
  require(f.physical() == before, "staging plan or rejected application wrote data");
  const auto result = f.store.rebase_stage_with_options("main", head, index, settings);
  const auto target = result["target"].get<std::string>();
  const auto revision = f.store.metadata(Kind::revision, target);
  require(result["target"] == plan["target"] && revision["parents"] == Json::array({head}) &&
              revision["files"]["code.cpp"]["manifest"] == manifest &&
              revision["files"]["code.cpp"]["executable"] == true &&
              f.store.refs().at("heads/main").id == head &&
              f.store.refs().at("archive/rebase-index/" + index).id == index &&
              text(read_bytes(root / "code.cpp")) == "later dirty work" &&
              text(read_bytes(root / "untracked")) == "keep" && f.store.native_head() == selected,
          "staging text publication changed working bytes, branch, mode or archive");
  const auto unchanged = f.physical();
  const auto fresh = f.store.rebase_stage_with_options("main", head, target, settings);
  require(fresh["state"] == "unchanged" && fresh["text_merge"]["attempted_files"] == 0 &&
              f.physical() == unchanged,
          "fresh staging text replay wrote data");
  const auto committed = f.store.commit_staged("main", "Rebased text selection");
  require(f.store.metadata(Kind::revision, committed)["files"] == revision["files"],
          "staged commit lost merged selection");
  f.store.gc(true);
  std::ostringstream output;
  f.store.reconstruct(manifest, output);
  require(output.str() == "A\nb\nC", "GC lost rebased index text");
  (void)f.store.closure({{Kind::revision, index}, {Kind::revision, base}}, true);
}
void range_limits(const fs::path &root) {
  Fixture f(root);
  const auto base = f.revision(Json{{"code", f.file("a\nb\n0\n")}});
  auto head = base;
  for (unsigned n = 1; n <= 65; ++n)
    head = f.revision(Json{{"code", f.file("a\nb\n" + std::to_string(n) + '\n')}}, Json::array({head}));
  const auto onto = f.revision(Json{{"code", f.file("A\nb\n0\n")}}, Json::array({base}));
  f.install(head);
  const auto before = f.physical();
  const auto settings = text_options();
  const auto plan = f.store.plan_rebase(root, "main", base, onto, settings);
  require(plan["ready"] == false && plan["completed_commits"] == 64 &&
              plan["text_merge"]["attempted_files"] == 64 &&
              plan["steps"].back()["text_merge"]["files"][0]["reason"] == "file_count_limit",
          "rebase reset the shared attempt budget between steps");
  fails([&] { f.store.rebase_directory(root, "main", head, base, onto, settings); });
  require(f.physical() == before, "late blocked rebase materialized earlier candidates");
}
void late_conflict(const fs::path &root) {
  Fixture f(root);
  const auto base = f.revision(Json{{"code", f.file("a\nb\nc\n")}});
  const auto first = f.revision(Json{{"code", f.file("a\nb\nC\n")}}, Json::array({base}));
  const auto head = f.revision(Json{{"code", f.file("Z\nb\nC\n")}}, Json::array({first}));
  const auto onto = f.revision(Json{{"code", f.file("A\nb\nc\n")}}, Json::array({base}));
  f.install(head);
  const auto before = f.physical();
  const auto settings = text_options();
  const auto plan = f.store.plan_rebase(root, "main", base, onto, settings);
  require(plan["ready"] == false && plan["completed_commits"] == 1 &&
              plan["steps"][1]["text_merge"]["files"][0]["status"] == "conflict",
          "overlap after a virtual step was hidden");
  fails([&] { f.store.rebase_directory(root, "main", head, base, onto, settings); });
  require(f.physical() == before, "late conflict wrote earlier merged objects");
}
void recovery(const fs::path &root, bool abort) {
  Fixture f(root);
  const auto base = f.revision(Json{{"code", f.file("a\nb\nc\n")}});
  const auto head = f.revision(Json{{"code", f.file("a\nb\nC\n")}}, Json::array({base}));
  const auto onto = f.revision(Json{{"code", f.file("A\nb\nc\n")}}, Json::array({base}));
  f.install(head);
  std::stop_source stop;
  fails([&] {
    f.store.rebase_directory(root, "main", head, base, onto, text_options(),
                             {stop.get_token(), [&](const ProgressEvent &event) {
                                if (event.phase == ProgressPhase::exporting && event.bytes)
                                  stop.request_stop();
                              }});
  });
  require(f.store.checkout_status()["pending"] == true, "text rebase omitted recovery journal");
  f.store.recover_checkout(abort);
  require(text(read_bytes(root / "code")) == (abort ? "a\nb\nC\n" : "A\nb\nC\n") &&
              (f.store.refs().at("heads/main").id == head) == abort,
          "text rebase resume/abort differs");
  f.store.gc(true);
  require(f.store.fsck() > 0, "text rebase recovery damaged retained objects");
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
          key->fill(62);
        }
        const auto suffix = std::to_string(encrypted) + '-' + std::to_string(layout);
        linear(temp.path / ("linear-" + suffix), key, layout);
        frozen(temp.path / ("index-" + suffix), key, layout);
      }
    range_limits(temp.path / "limits");
    late_conflict(temp.path / "conflict");
    recovery(temp.path / "resume", false);
    recovery(temp.path / "abort", true);
    std::cout << "Virtual text rebases, shared range budgets, atomic frozen index replay and recovery\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
