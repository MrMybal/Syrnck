#include "native_history_fixture.hpp"
#include "native_merge.hpp"
#include "native_rebase.hpp"
#include "native_stage_rebase.hpp"
#include <iostream>
#include <chrono>
#include <thread>
using namespace native_test;
namespace {
std::string code() {
  std::string result;
  for (unsigned i = 0; i < 100; ++i)
    result += "int value_" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
  return result;
}
std::string edit(std::string text, unsigned line, unsigned value) {
  const auto old = "= " + std::to_string(line) + ";";
  text.replace(text.find(old), old.size(), "= " + std::to_string(value) + ";");
  return text;
}
void merge(const fs::path &root, std::optional<Key> key, unsigned layout, unsigned side) {
  Fixture f(root, key);
  const auto original = code(), ours = edit(original, 20, 98), theirs = edit(original, 70, 99);
  const std::string old = "Source/Old.cpp", next = "Source/New.cpp";
  const auto base = f.revision(Json{{old, f.file(original)}});
  const auto current = f.revision(Json{{side == 1 ? old : next, f.file(ours, true)}}, Json::array({base}));
  const auto source = f.revision(Json{{side == 0 ? old : next, f.file(theirs)}}, Json::array({base}));
  f.install(current);
  f.store.set_ref("heads/source", {Kind::revision, source});
  f.pack(layout);
  const auto before = f.physical();
  NativeMergeOptions settings;
  settings.renames.emplace();
  settings.text_merge.emplace();
  const auto plan = prepare_native_merge(f.store, root, "main", source, settings, true, {});
  require(plan.report["ready"] == true && plan.files.size() == 1 && plan.files.contains(next) &&
              plan.files.at(next).executable && plan.report["renames"]["renames"].size() == 1 &&
              plan.generated.at(plan.files.at(next).manifest) == edit(ours, 70, 99),
          "rename/edit merge lost mapped content, mode or path");
  auto limited = settings;
  limited.renames->max_work = 1;
  fails([&] { f.store.plan_merge(root, "main", source, limited); });
  require(f.physical() == before, "rename merge planning or a limit changed the workspace");
  const auto result =
      f.store.merge_directory(root, "main", current, source, "merge renamed source", settings);
  require(!fs::exists(root / path_from_utf8(old)) &&
              text(read_bytes(root / path_from_utf8(next))) == edit(ours, 70, 99),
          "applied rename merge differs from its plan");
  const auto revision = f.store.metadata(Kind::revision, result["target"].get<std::string>());
  require(revision["parents"] == Json::array({current, source}) && revision["files"].size() == 1,
          "rename merge changed parent order or retained an obsolete path");
  f.store.remove_ref("heads/source");
  f.store.gc(true);
  require(f.store.metadata(Kind::revision, base)["files"].contains(old) && f.store.fsck() > 0,
          "rename merge lost retained source history");
}
void structural(const fs::path &root, std::string_view scenario) {
  Fixture f(root);
  const auto original = code(), changed = edit(original, 20, 98);
  const auto base = f.revision(Json{{"old.cpp", f.file(original)}});
  Json ours{{"new.cpp", f.file(changed)}}, theirs = Json::object();
  std::string reason;
  if (scenario == "different-names") {
    theirs = Json{{"other.cpp", f.file(original)}};
    reason = "rename_rename";
  } else if (scenario == "rename-delete")
    reason = "rename_delete";
  else if (scenario == "delete-rename") {
    ours = Json::object();
    theirs = Json{{"new.cpp", f.file(changed)}};
    reason = "delete_rename";
  } else {
    theirs = Json{{"old.cpp", f.file(original)}, {"new.cpp", f.file("unrelated new file")}};
    reason = "rename_add";
  }
  const auto current = f.revision(ours, Json::array({base})),
             source = f.revision(theirs, Json::array({base}));
  f.install(current);
  f.store.set_ref("heads/source", {Kind::revision, source});
  NativeMergeOptions settings;
  settings.renames.emplace();
  settings.text_merge.emplace();
  settings.conflict_copies = true;
  const auto before = f.physical();
  const auto blocked = f.store.plan_merge(root, "main", source, settings);
  require(blocked["ready"] == false, "structural rename conflict became an implicit merge");
  bool found = false;
  for (const auto &row : blocked["conflicts"])
    if (row["reason"] == reason)
      found = true;
  require(found, "structural rename conflict reason was lost");
  fails([&] { f.store.merge_directory(root, "main", current, source, "must block", settings); });
  require(f.physical() == before, "blocked rename merge published state");
  auto restore_base = settings;
  restore_base.resolutions = NativeResolutionSet::parse(blocked["resolution_context"]);
  for (const auto &row : blocked["conflicts"])
    restore_base.resolutions->files[row["path"].get<std::string>()] = {
        row["path"] == "old.cpp" ? NativeResolutionChoice::base : NativeResolutionChoice::remove};
  const auto restored = prepare_native_merge(f.store, root, "main", source, restore_base, true, {});
  require(restored.report["ready"] == true && restored.files.size() == 1 &&
              restored.files.contains("old.cpp") &&
              restored.files.at("old.cpp").manifest ==
              f.store.metadata(Kind::revision, base)["files"]["old.cpp"]["manifest"].get<std::string>(),
          "structural rename resolution cannot restore the original baseline path");
  settings.resolutions = NativeResolutionSet::parse(blocked["resolution_context"]);
  for (const auto &row : blocked["conflicts"])
    settings.resolutions->files[row["path"].get<std::string>()] = {NativeResolutionChoice::current};
  const auto chosen = f.store.plan_merge(root, "main", source, settings);
  require(chosen["ready"] == true, "explicit path resolutions did not resolve a structural rename conflict");
  const auto result =
      f.store.merge_directory(root, "main", current, source, "keep current rename choice", settings);
  require(f.store.metadata(Kind::revision, result["target"].get<std::string>())["files"] == ours,
          "structural rename resolution ignored current path choices");
}
void namespace_guards(const fs::path &root, bool prefix) {
  Fixture f(root);
  const auto original = code();
  const auto base = f.revision(Json{{"old.cpp", f.file(original)}});
  const auto current =
      f.revision(Json{{"Folder/File.cpp", f.file(edit(original, 20, 98))}}, Json::array({base}));
  const auto source =
      f.revision(Json{{"old.cpp", f.file(edit(original, 70, 99))},
                      {prefix ? "Folder" : "Folder/file.cpp", f.file("independent obstruction")}},
                 Json::array({base}));
  f.install(current);
  f.store.set_ref("heads/source", {Kind::revision, source});
  NativeMergeOptions settings;
  settings.renames.emplace();
  settings.text_merge.emplace();
  const auto before = f.physical();
  const auto plan = f.store.plan_merge(root, "main", source, settings);
  bool found = false;
  for (const auto &row : plan["conflicts"])
    if (row["reason"] == (prefix ? "file_directory_collision" : "case_collision"))
      found = true;
  require(plan["ready"] == false && found, "rename alignment bypassed a portable namespace guard");
  fails([&] { f.store.merge_directory(root, "main", current, source, "blocked namespace", settings); });
  require(f.physical() == before, "rename namespace conflict modified stored or working files");
}
void unchanged_names(const fs::path &root) {
  Fixture f(root);
  const auto original = code();
  const auto base = f.revision(Json{{"same.cpp", f.file(original)}});
  const auto current = f.revision(Json{{"same.cpp", f.file(edit(original, 20, 98))}}, Json::array({base}));
  const auto source = f.revision(Json{{"same.cpp", f.file(edit(original, 70, 99))}}, Json::array({base}));
  f.install(current);
  NativeMergeOptions settings;
  settings.renames.emplace();
  settings.text_merge.emplace();
  const auto plan = prepare_native_merge(f.store, root, "main", source, settings, true, {});
  require(plan.report["ready"] == true && plan.files.size() == 1 && plan.files.contains("same.cpp") &&
              plan.report["renames"]["renames"].empty() &&
              plan.report["renames"]["incoming"]["input_bytes"] == 0 &&
              plan.generated.at(plan.files.at("same.cpp").manifest) == edit(edit(original, 20, 98), 70, 99),
          "enabling similarity changed a merge without renamed candidates");
}
void mapped_resolution(const fs::path &root) {
  Fixture f(root);
  const auto original = code(), ours = edit(original, 37, 98), theirs = edit(original, 37, 99);
  const auto base = f.revision(Json{{"old.cpp", f.file(original)}});
  const auto current = f.revision(Json{{"new.cpp", f.file(ours)}}, Json::array({base}));
  const auto source = f.revision(Json{{"old.cpp", f.file(theirs)}}, Json::array({base}));
  f.install(current);
  f.store.set_ref("heads/source", {Kind::revision, source});
  NativeMergeOptions settings;
  settings.renames.emplace();
  settings.text_merge.emplace();
  const auto blocked = f.store.plan_merge(root, "main", source, settings);
  require(blocked["ready"] == false && blocked["conflicts"][0]["path"] == "new.cpp" &&
              blocked["conflicts"][0]["reason"] == "content",
          "mapped content conflict used the obsolete name");
  settings.resolutions = NativeResolutionSet::parse(blocked["resolution_context"]);
  settings.resolutions->files["new.cpp"] = {NativeResolutionChoice::incoming};
  const auto result =
      f.store.merge_directory(root, "main", current, source, "incoming bytes under renamed path", settings);
  require(!fs::exists(root / "old.cpp") && text(read_bytes(root / "new.cpp")) == theirs &&
              result["renames"].is_object(),
          "incoming choice did not use the aligned source file");
}
void rebase_virtual(const fs::path &root, std::optional<Key> key, unsigned layout) {
  Fixture f(root, key);
  const auto original = code(), first_text = edit(original, 70, 99), final_text = edit(first_text, 80, 98);
  const auto base = f.revision(Json{{"old.cpp", f.file(original)}});
  const auto first = f.revision(Json{{"old.cpp", f.file(first_text)}}, Json::array({base}));
  const auto head = f.revision(Json{{"old.cpp", f.file(final_text, true)}}, Json::array({first}));
  const auto onto = f.revision(Json{{"new.cpp", f.file(edit(original, 20, 97))}}, Json::array({base}));
  f.install(head);
  f.store.set_ref("heads/onto", {Kind::revision, onto});
  f.store.set_ref("heads/original", {Kind::revision, head});
  f.pack(layout);
  NativeRebaseOptions settings;
  settings.renames.emplace();
  settings.text_merge.emplace();
  const auto before = f.physical();
  const auto plan = prepare_native_rebase(f.store, root, "main", base, onto, settings, true, {});
  const auto expected = edit(final_text, 20, 97);
  require(plan.report["ready"] == true && plan.report["completed_commits"] == 2 &&
              plan.generated.size() == 2 && plan.files.size() == 1 && plan.files.contains("new.cpp") &&
              plan.generated.at(plan.files.at("new.cpp").manifest) == expected &&
              plan.files.at("new.cpp").executable &&
              plan.report["steps"][1]["renames"]["renames"].size() == 1,
          "multi-step rebase did not follow an upstream edited rename through a virtual file");
  require(f.physical() == before, "rename rebase planning materialized virtual data");
  const auto result = f.store.rebase_directory(root, "main", head, base, onto, settings);
  require(result["target"] == plan.target && !fs::exists(root / "old.cpp") &&
              text(read_bytes(root / "new.cpp")) == expected,
          "rebase regeneration changed mapped virtual candidates");
  f.store.gc(true);
  require(f.store.metadata(Kind::revision, head)["files"].contains("old.cpp") && f.store.fsck() > 0,
          "rename rebase damaged protected history");
}
void staging(const fs::path &root, std::optional<Key> key, unsigned layout, bool representation_only) {
  Fixture f(root, key);
  const auto original = code();
  const auto staged_text = representation_only ? original : edit(original, 70, 99);
  const auto upstream_text = representation_only ? original : edit(original, 20, 98);
  const auto baseline_file = f.file(original, false, 1024);
  const auto staged_file = f.file(staged_text, false, 2048);
  const auto base = f.revision(Json{{"old.cpp", baseline_file}});
  const auto head = f.revision(Json{{"new.cpp", f.file(upstream_text, false, 1024)}}, Json::array({base}));
  const auto index = f.store
                         .put(Kind::revision, bytes(Json{{"version", 1},
                                                         {"parents", Json::array({base})},
                                                         {"message", "Syrnck staging v1: main"},
                                                         {"timestamp", 0},
                                                         {"files", Json{{"old.cpp", staged_file}}}}
                                                        .dump()))
                         .first.id;
  f.install(head);
  f.store.set_ref("staging/main", {Kind::revision, index});
  f.write("new.cpp", "uncaptured working edits");
  f.pack(layout);
  const auto before = f.physical();
  NativeStageRebaseOptions settings;
  settings.renames.emplace();
  settings.text_merge.emplace();
  const NativeRewriteState state(f.store, "main", {});
  const auto plan = prepare_native_stage_rebase(f.store, state, true, {}, nullptr, &*settings.text_merge,
                                                &*settings.renames);
  require(plan.report["ready"] == true && plan.candidate["files"].size() == 1 &&
              plan.candidate["files"].contains("new.cpp") && f.physical() == before,
          "stale index did not follow a renamed upstream path without changing working files");
  if (representation_only)
    require(plan.candidate["files"]["new.cpp"]["manifest"] == staged_file["manifest"],
            "rename-aware index rebase discarded an explicitly staged representation");
  const auto applied = f.store.rebase_stage_with_options("main", head, index, settings);
  const auto next = applied["index"].get<std::string>();
  const auto metadata = f.store.metadata(Kind::revision, next);
  std::ostringstream output;
  f.store.reconstruct(metadata["files"]["new.cpp"]["manifest"].get<std::string>(), output);
  require(next == plan.target &&
              output.str() == (representation_only ? original : edit(staged_text, 20, 98)) &&
              text(read_bytes(root / "new.cpp")) == "uncaptured working edits" &&
              f.store.refs().at("heads/main").id == head &&
              f.store.refs().at("archive/rebase-index/" + index).id == index,
          "rename-aware index publication lost frozen content, workspace edits or archived staging");
  f.store.gc(true);
  require(f.store.fsck() > 0, "renamed index rebase damaged retained versions");
}
void replay_recovery(const fs::path &root, NativeReplayKind kind, bool abort) {
  Fixture f(root);
  const auto original = code(), source_text = edit(original, 70, 99);
  const auto base = f.revision(Json{{"old.cpp", f.file(original)}});
  const auto source = f.revision(Json{{"new.cpp", f.file(source_text)}}, Json::array({base}));
  const bool inverse = kind == NativeReplayKind::revert;
  const auto before = edit(inverse ? source_text : original, 20, 98);
  const auto current = f.revision(Json{{inverse ? "new.cpp" : "old.cpp", f.file(before)}},
                                  Json::array({inverse ? source : base}));
  f.install(current);
  f.store.set_ref("heads/source", {Kind::revision, source});
  NativeReplayOptions settings;
  settings.renames.emplace();
  settings.text_merge.emplace();
  require(f.store.plan_replay_with_options(root, "main", source, kind, settings)["ready"] == true,
          "rename-aware cherry-pick/revert is blocked");
  std::stop_source stopped;
  fails([&] {
    f.store.replay_directory_with_options(root, "main", current, source, kind, "recover rename replay",
                                          settings,
                                          {stopped.get_token(), [&](const ProgressEvent &event) {
                                             if (event.phase == ProgressPhase::exporting && event.bytes)
                                               stopped.request_stop();
                                           }});
  });
  require(f.store.checkout_status()["pending"] == true, "rename replay did not freeze a recoverable target");
  f.store.recover_checkout(abort);
  const auto final_name = abort ? (inverse ? "new.cpp" : "old.cpp") : (inverse ? "old.cpp" : "new.cpp");
  require(text(read_bytes(root / final_name)) ==
                  (abort ? before : edit(inverse ? original : source_text, 20, 98)) &&
              (f.store.refs().at("heads/main").id == current) == abort,
          "rename replay recovery lost bytes or advanced the wrong HEAD");
  f.store.gc(true);
  require(f.store.fsck() > 0, "rename replay recovery damaged retained objects");
}
int crash_probe(const fs::path &root, NativeReplayKind kind, bool encrypted) {
  require(!fs::exists(root), "rename crash fixture directory already exists");
  std::optional<Key> key;
  if (encrypted) {
    key.emplace();
    key->fill(75);
  }
  Fixture f(root, key);
  const auto original = code(), source_text = edit(original, 70, 99);
  const auto base = f.revision(Json{{"old.cpp", f.file(original)}});
  const auto source = f.revision(Json{{"new.cpp", f.file(source_text)}}, Json::array({base}));
  const bool inverse = kind == NativeReplayKind::revert;
  const auto before = edit(inverse ? source_text : original, 20, 98);
  const auto current = f.revision(Json{{inverse ? "new.cpp" : "old.cpp", f.file(before)}},
                                  Json::array({inverse ? source : base}));
  f.install(current);
  f.store.set_ref("heads/source", {Kind::revision, source});
  NativeReplayOptions settings;
  settings.renames.emplace();
  settings.text_merge.emplace();
  std::cout << Json{{"event", "prepared"}, {"head", current}}.dump() << '\n' << std::flush;
  f.store.replay_directory_with_options(root, "main", current, source, kind, "crash rename replay", settings,
                                        {{}, [&](const ProgressEvent &event) {
                                           if (event.phase == ProgressPhase::exporting && event.bytes) {
                                             std::cout << "{\"event\":\"checkpoint\"}\n" << std::flush;
                                             for (;;)
                                               std::this_thread::sleep_for(std::chrono::seconds(1));
                                           }
                                         }});
  throw Error("rename crash checkpoint was not reached");
}
} // namespace
int main(int argc, char **argv) {
  try {
    if (argc == 5 && std::string_view(argv[1]) == "--crash-probe") {
      const std::string_view kind(argv[3]), mode(argv[4]);
      require((kind == "cherry-pick" || kind == "revert") && (mode == "plain" || mode == "encrypted"),
              "invalid rename crash fixture mode");
      return crash_probe(path_from_utf8(argv[2]),
                         kind == "revert" ? NativeReplayKind::revert : NativeReplayKind::cherry_pick,
                         mode == "encrypted");
    }
    require(argc == 1, "unexpected rename test arguments");
    Temporary temporary;
    for (bool encrypted : {false, true})
      for (unsigned layout = 0; layout < 3; ++layout) {
        std::optional<Key> key;
        if (encrypted) {
          key.emplace();
          key->fill(74);
        }
        for (unsigned side = 0; side < 3; ++side)
          merge(temporary.path / ("merge-" + std::to_string(encrypted) + '-' + std::to_string(layout) + '-' +
                                  std::to_string(side)),
                key, layout, side);
        rebase_virtual(temporary.path /
                           ("rebase-" + std::to_string(encrypted) + '-' + std::to_string(layout)),
                       key, layout);
        for (bool representation : {false, true})
          staging(temporary.path / ("index-" + std::to_string(encrypted) + '-' + std::to_string(layout) +
                                    '-' + std::to_string(representation)),
                  key, layout, representation);
      }
    for (const auto scenario : {"different-names", "rename-delete", "delete-rename", "rename-add"})
      structural(temporary.path / scenario, scenario);
    mapped_resolution(temporary.path / "mapped-resolution");
    unchanged_names(temporary.path / "unchanged-names");
    namespace_guards(temporary.path / "namespace-prefix", true);
    namespace_guards(temporary.path / "namespace-case", false);
    for (auto kind : {NativeReplayKind::cherry_pick, NativeReplayKind::revert})
      for (bool abort : {false, true})
        replay_recovery(temporary.path /
                            (std::to_string(static_cast<unsigned>(kind)) + '-' + std::to_string(abort)),
                        kind, abort);
    std::cout << "Rename-aware merges, explicit structural choices, virtual rebases and replay recovery\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
