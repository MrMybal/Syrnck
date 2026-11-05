#include "native_history_fixture.hpp"
#include "native_replay.hpp"
#include <iostream>
using namespace native_test;
namespace {
void planning(const fs::path &root, std::optional<Key> key, unsigned layout, NativeReplayKind kind) {
  Fixture f(root, key);
  const bool inverse = kind == NativeReplayKind::revert;
  const auto a = f.file("one\r\ntwo\r\nlast"), b = f.file("one\r\ntwo\r\nLAST"),
             ours = f.file(inverse ? "ONE\r\ntwo\r\nLAST" : "ONE\r\ntwo\r\nlast", true);
  const auto base = f.revision(Json{{"code.cpp", a}});
  const auto source = f.revision(Json{{"code.cpp", b}}, Json::array({base}));
  const auto current = f.revision(Json{{"code.cpp", ours}}, Json::array({inverse ? source : base}));
  f.install(current);
  f.store.set_ref("heads/feature", {Kind::revision, source});
  f.pack(layout);
  const auto before = f.physical();
  require(f.store.plan_replay(root, "main", source, kind)["ready"] == false,
          "replay text merge became implicit");
  require(f.store.plan_replay_with_options(root, "main", source, kind, {}) ==
              f.store.plan_replay(root, "main", source, kind),
          "unconfigured replay changed legacy behavior");
  NativeReplayOptions settings;
  settings.text_merge.emplace();
  const auto plan = prepare_native_replay(f.store, root, "main", source, kind, {}, true, {}, nullptr,
                                          &*settings.text_merge);
  const auto report = f.store.plan_replay_with_options(root, "main", source, kind, settings, true);
  const auto expected = inverse ? "ONE\r\ntwo\r\nlast" : "ONE\r\ntwo\r\nLAST";
  require(report == plan.report && report["ready"] == true && report["empty"] == false &&
              report["text_merge"]["files"][0]["status"] == "clean" && plan.generated.size() == 1 &&
              plan.generated.at(plan.files.at("code.cpp").manifest) == expected &&
              plan.files.at("code.cpp").executable &&
              !f.store.contains({Kind::manifest, plan.files.at("code.cpp").manifest}),
          "replay virtual text or inverse delta differs");
  require(report["resolution_context"]["base"] == (inverse ? source : base) &&
              report["resolution_context"]["parent"] == base && report["parent"] == base,
          "replay text context lost its direction");
  auto explicit_choice = settings;
  explicit_choice.resolutions = NativeResolutionSet::parse(report["resolution_context"]);
  explicit_choice.resolutions->files["code.cpp"] = {NativeResolutionChoice::current};
  const auto selected = f.store.plan_replay_with_options(root, "main", source, kind, explicit_choice);
  require(selected["ready"] == false && selected["empty"] == true && selected["conflicts"].empty() &&
              selected["text_merge"]["attempted_files"] == 0 &&
              selected["resolved"][0]["choice"] == "current",
          "automatic replay overrode explicit current choice or created an empty commit");
  explicit_choice.resolutions->operation = inverse ? "cherry-pick" : "revert";
  fails([&] { f.store.plan_replay_with_options(root, "main", source, kind, explicit_choice); });
  auto limited = settings;
  limited.text_merge->max_work = 1;
  require(f.store.plan_replay_with_options(root, "main", source, kind,
                                           limited)["text_merge"]["files"][0]["reason"] == "work_limit",
          "replay ignored text work limit");
  limited.text_merge->max_file_bytes = 1;
  require(f.store.plan_replay_with_options(root, "main", source, kind,
                                           limited)["text_merge"]["files"][0]["reason"] == "file_limit",
          "replay ignored file limit");
  std::stop_source stop;
  fails([&] {
    f.store.plan_replay_with_options(root, "main", source, kind, settings, false,
                                     {stop.get_token(), [&](const ProgressEvent &event) {
                                        if (event.phase == ProgressPhase::reconstructing && event.bytes)
                                          stop.request_stop();
                                      }});
  });
  require(f.physical() == before, "replay text planning wrote data");
  f.write("code.cpp", "dirty");
  require(f.store.plan_replay_with_options(root, "main", source, kind, settings)["ready"] == false,
          "text replay bypassed dirty workspace");
}
void mainlines(const fs::path &root) {
  Fixture f(root);
  const auto base = f.revision(Json{{"code", f.file("a\nb\nc\n")}});
  const auto other = f.revision(Json{{"code", f.file("a\nB\nc\n")}}, Json::array({base}));
  const auto source = f.revision(Json{{"code", f.file("a\nB\nC\n")}}, Json::array({base, other}));
  const auto current = f.revision(Json{{"code", f.file("A\nB\nc\n")}}, Json::array({other}));
  f.install(current);
  NativeReplayOptions settings;
  settings.text_merge.emplace();
  fails([&] {
    f.store.plan_replay_with_options(root, "main", source, NativeReplayKind::cherry_pick, settings);
  });
  settings.mainline = 2;
  const auto plan =
      f.store.plan_replay_with_options(root, "main", source, NativeReplayKind::cherry_pick, settings);
  require(plan["ready"] == true && plan["parent"] == other && plan["mainline"] == 2,
          "text cherry-pick ignored selected merge parent");
  const auto after = f.revision(Json{{"code", f.file("A\nB\nC\n")}}, Json::array({source}));
  f.install(after);
  const auto inverse =
      f.store.plan_replay_with_options(root, "main", source, NativeReplayKind::revert, settings);
  require(inverse["ready"] == true && inverse["resolution_context"]["base"] == source &&
              inverse["resolution_context"]["parent"] == other &&
              inverse["text_merge"]["files"][0]["status"] == "clean",
          "text revert used the wrong merge parent");
  settings.mainline = 3;
  fails([&] { f.store.plan_replay_with_options(root, "main", source, NativeReplayKind::revert, settings); });
  settings.mainline = {};
  settings.text_merge->max_lines = 0;
  fails([&] { f.store.plan_replay_with_options(root, "main", source, NativeReplayKind::revert, settings); });
}
void application(const fs::path &root, std::optional<Key> key, unsigned layout, NativeReplayKind kind) {
  Fixture f(root, key);
  const bool inverse = kind == NativeReplayKind::revert;
  const auto original = f.file("a\r\nb\r\nc"), changed = f.file("a\r\nb\r\nC"),
             our_file = f.file(inverse ? "A\r\nb\r\nC" : "A\r\nb\r\nc", true);
  const auto base = f.revision(Json{{"code.cpp", original}});
  const auto source = f.revision(Json{{"code.cpp", changed}}, Json::array({base}));
  const auto current = f.revision(Json{{"code.cpp", our_file}}, Json::array({inverse ? source : base}));
  f.install(current);
  f.store.set_ref("heads/source", {Kind::revision, source});
  f.pack(layout);
  NativeReplayOptions settings;
  settings.text_merge.emplace();
  const auto expected = inverse ? "A\r\nb\r\nc" : "A\r\nb\r\nC";
  const auto plan = f.store.plan_replay_with_options(root, "main", source, kind, settings, true);
  const auto manifest = plan["text_merge"]["files"][0]["target"]["manifest"].get<std::string>();
  const auto before = f.physical();
  fails([&] {
    f.store.replay_directory_with_options(root, "main", base, source, kind, "wrong head", settings);
  });
  auto empty = settings;
  empty.resolutions = NativeResolutionSet::parse(plan["resolution_context"]);
  empty.resolutions->files["code.cpp"] = {NativeResolutionChoice::current};
  fails([&] { f.store.replay_directory_with_options(root, "main", current, source, kind, "empty", empty); });
  std::stop_source stopped;
  fails([&] {
    f.store.replay_directory_with_options(root, "main", current, source, kind, "cancel before chunks",
                                          settings, {stopped.get_token(), [&](const ProgressEvent &event) {
                                                       if (event.phase == ProgressPhase::ingesting)
                                                         stopped.request_stop();
                                                     }});
  });
  require(f.physical() == before, "rejected or cancelled replay published objects");
  f.write("code.cpp", "dirty working bytes");
  const auto dirty = f.physical();
  fails(
      [&] { f.store.replay_directory_with_options(root, "main", current, source, kind, "dirty", settings); });
  require(f.physical() == dirty && !f.store.contains({Kind::manifest, manifest}),
          "blocked text replay wrote a candidate");
  f.install(current);
  const auto result =
      f.store.replay_directory_with_options(root, "main", current, source, kind, "replayed text", settings);
  const auto next = result["target"].get<std::string>();
  require(text(read_bytes(root / "code.cpp")) == expected &&
              f.store.metadata(Kind::revision, next)["parents"] == Json::array({current}) &&
              f.store.metadata(Kind::revision, next)["files"]["code.cpp"]["manifest"] == manifest &&
              f.store.refs().at("heads/source").id == source,
          "text replay changed ancestry, source or bytes");
  f.store.remove_ref("heads/source");
  f.store.gc(true);
  std::ostringstream output;
  f.store.reconstruct(manifest, output);
  require(output.str() == expected && f.store.metadata(Kind::revision, current)["files"].contains("code.cpp"),
          "GC lost text replay or its parent");
  // Cherry-pick keeps its complete result without retaining source ancestry.
  if (!inverse)
    require(!f.store.contains({Kind::revision, source}), "fixture retained an unrelated replay source");
}
void recovery(const fs::path &root, NativeReplayKind kind, bool abort) {
  Fixture f(root);
  const bool inverse = kind == NativeReplayKind::revert;
  const auto base = f.revision(Json{{"code.cpp", f.file("a\nb\nc\n")}});
  const auto source = f.revision(Json{{"code.cpp", f.file("a\nb\nC\n")}}, Json::array({base}));
  const auto current = f.revision(Json{{"code.cpp", f.file(inverse ? "A\nb\nC\n" : "A\nb\nc\n")}},
                                  Json::array({inverse ? source : base}));
  f.install(current);
  f.store.set_ref("heads/source", {Kind::revision, source});
  NativeReplayOptions settings;
  settings.text_merge.emplace();
  std::stop_source stopped;
  fails([&] {
    f.store.replay_directory_with_options(root, "main", current, source, kind, "recover text", settings,
                                          {stopped.get_token(), [&](const ProgressEvent &event) {
                                             if (event.phase == ProgressPhase::exporting && event.bytes)
                                               stopped.request_stop();
                                           }});
  });
  require(f.store.checkout_status()["pending"] == true, "text replay omitted its recovery journal");
  f.store.recover_checkout(abort);
  require(text(read_bytes(root / "code.cpp")) ==
                  (abort ? (inverse ? "A\nb\nC\n" : "A\nb\nc\n") : (inverse ? "A\nb\nc\n" : "A\nb\nC\n")) &&
              (f.store.refs().at("heads/main").id == current) == abort &&
              f.store.refs().at("heads/source").id == source,
          "text replay resume/abort lost bytes or source branch");
  f.store.gc(true);
  require(f.store.fsck() > 0, "text replay recovery damaged retained history");
}
} // namespace
int main() {
  Temporary tmp;
  try {
    for (bool encrypted : {false, true})
      for (unsigned layout = 0; layout < 3; ++layout)
        for (auto kind : {NativeReplayKind::cherry_pick, NativeReplayKind::revert}) {
          std::optional<Key> key;
          if (encrypted) {
            key.emplace();
            key->fill(37);
          }
          planning(tmp.path / (std::to_string(encrypted) + '-' + std::to_string(layout) + '-' +
                               std::to_string(static_cast<int>(kind))),
                   key, layout, kind);
          application(tmp.path / ("apply-" + std::to_string(encrypted) + '-' + std::to_string(layout) + '-' +
                                  std::to_string(static_cast<int>(kind))),
                      key, layout, kind);
        }
    mainlines(tmp.path / "mainline");
    for (auto kind : {NativeReplayKind::cherry_pick, NativeReplayKind::revert})
      for (bool abort : {false, true})
        recovery(tmp.path /
                     ("recover-" + std::to_string(static_cast<int>(kind)) + '-' + std::to_string(abort)),
                 kind, abort);
    std::cout << "Bounded text cherry-pick/revert plans and application, inverse direction, explicit "
                 "precedence, mainlines, retained results and recovery\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
