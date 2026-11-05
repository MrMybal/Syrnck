#include "native_history_fixture.hpp"
#include <iostream>
using namespace native_test;
namespace {
void schema() {
  NativeResolutionSet set{"merge", std::string(64, '0'), std::string(64, '1'), std::string(64, '2'), {}, {}};
  set.files["current"] = {NativeResolutionChoice::current};
  set.files["incoming"] = {NativeResolutionChoice::incoming};
  set.files["base"] = {NativeResolutionChoice::base};
  set.files["removed"] = {NativeResolutionChoice::remove};
  set.files["custom"] = {NativeResolutionChoice::file, std::string(64, '3'), true};
  const auto document = set.json();
  require(NativeResolutionSet::parse(document).json() == document &&
              NativeResolutionSet::decode(bytes(document.dump(2))).json() == document,
          "resolution document roundtrip differs");
  for (const auto &bad :
       {Json{{"version", 2}}, Json{{"extra", 1}}, Json{{"operation", "force"}}, Json{{"head", "HEAD"}},
        Json{{"parent", "HEAD"}}, Json{{"files", Json::array()}},
        Json{{"files", Json{{"../escape", Json{{"choice", "current"}}}}}},
        Json{{"files", Json{{"asset", Json{{"choice", "file"}, {"manifest", std::string(64, '0')}}}}}},
        Json{{"files", Json{{"asset", Json{{"choice", "current"}, {"executable", false}}}}}}}) {
    auto value = document;
    value.update(bad);
    fails([&] { NativeResolutionSet::parse(value); });
  }
  auto duplicated = document.dump();
  duplicated.insert(duplicated.size() - 1, ",\"operation\":\"revert\"");
  fails([&] { NativeResolutionSet::decode(bytes(duplicated)); });
  set.files.clear();
  auto nested = set.json().dump();
  const auto at = nested.find("\"files\":{}");
  nested.replace(at, std::string_view("\"files\":{}").size(),
                 R"("files":{"asset":{"choice":"current"},"asset":{"choice":"incoming"}})");
  fails([&] { NativeResolutionSet::decode(bytes(nested)); });
  fails([&] { NativeResolutionSet::decode(bytes(std::string(65, '[') + "0" + std::string(65, ']'))); });
  fails([&] { NativeResolutionSet::decode(bytes(std::string(max_object_size + 1, 'x'))); });
  fails([&] { NativeResolutionSet::decode(bytes("{")); });
}
void choices(const fs::path &path, std::optional<Key> key, unsigned physical) {
  Fixture f(path, key);
  const auto a = f.file("base"), ours = f.file("ours"), theirs = f.file("incoming"),
             own = f.file("independent");
  const auto custom = f.file("manually combined", true);
  const auto base = f.revision(Json{{"asset", a}});
  const auto current = f.revision(Json{{"asset", ours}, {"own", own}}, Json::array({base}));
  const auto source = f.revision(Json{{"asset", theirs}}, Json::array({base}));
  f.install(current);
  f.store.set_ref("heads/feature", {Kind::revision, source});
  f.pack(physical);
  const auto report = f.store.plan_merge(path, "main", source);
  auto set = NativeResolutionSet::parse(report["resolution_context"]);
  require(!report["ready"].get<bool>() && report["conflicts"].size() == 1,
          "resolution fixture has no conflict");
  const auto before = f.physical();
  NativeMergeOptions settings;
  settings.resolutions = set;
  settings.resolutions->head = source;
  fails([&] { f.store.plan_merge(path, "main", source, settings); });
  settings.resolutions = set;
  settings.resolutions->files["own"] = {NativeResolutionChoice::incoming};
  fails([&] { f.store.plan_merge(path, "main", source, settings); });
  require(f.physical() == before, "stale or non-conflict resolution changed data");
  std::vector<std::string> retained;
  for (const auto choice :
       {NativeResolutionChoice::current, NativeResolutionChoice::incoming, NativeResolutionChoice::base,
        NativeResolutionChoice::remove, NativeResolutionChoice::file}) {
    set.files["asset"] = {choice};
    if (choice == NativeResolutionChoice::file)
      set.files["asset"] = {choice, custom["manifest"].get<std::string>(), true};
    settings.resolutions = set;
    const auto snapshot = f.physical();
    const auto plan = f.store.plan_merge(path, "main", source, settings, true);
    require(plan["ready"] == true && plan["conflicts"].empty() && plan["resolved"].size() == 1 &&
                plan["objects_verified"] == true && f.physical() == snapshot,
            "resolved plan wrote files or retained conflicts");
    std::stop_source stop;
    fails([&] {
      f.store.merge_directory(path, "main", current, source, "cancelled", settings,
                              {stop.get_token(), [&](const ProgressEvent &) { stop.request_stop(); }});
    });
    require(f.physical() == snapshot, "cancelled resolved merge wrote data");
    const auto result = f.store.merge_directory(
        path, "main", current, source, "resolved " + std::to_string(static_cast<int>(choice)), settings);
    const auto target = result["target"].get<std::string>();
    retained.push_back(target);
    require(f.store.metadata(Kind::revision, target)["parents"] == Json::array({current, source}) &&
                text(read_bytes(path / "own")) == "independent",
            "resolved merge lost parents or independent file");
    if (choice == NativeResolutionChoice::remove)
      require(!fs::exists(path / "asset"), "explicit removal retained the file");
    else {
      const auto expected = choice == NativeResolutionChoice::current    ? "ours"
                            : choice == NativeResolutionChoice::incoming ? "incoming"
                            : choice == NativeResolutionChoice::base     ? "base"
                                                                         : "manually combined";
      require(text(read_bytes(path / "asset")) == expected, "explicit file selection differs");
    }
    f.install(current);
  }
  f.store.gc(true);
  for (const auto &target : retained)
    (void)f.store.metadata(Kind::revision, target);
  const auto custom_revision = f.store.metadata(Kind::revision, retained.back());
  require(custom_revision["files"]["asset"] == custom && f.store.fsck() > 0, "GC lost a custom resolution");
  auto replay = NativeResolutionSet::parse(
      f.store.plan_replay(path, "main", source, NativeReplayKind::cherry_pick)["resolution_context"]);
  replay.files["asset"] = {NativeResolutionChoice::current};
  const auto empty =
      f.store.plan_replay_resolved(path, "main", source, NativeReplayKind::cherry_pick, replay);
  require(empty["empty"] == true && empty["ready"] == false,
          "current-only replay silently created an empty commit");
  replay.files["asset"] = {NativeResolutionChoice::incoming};
  const auto picked = f.store.replay_directory_resolved(
      path, "main", current, source, NativeReplayKind::cherry_pick, "resolved pick", replay);
  require(f.store.metadata(Kind::revision, picked["target"].get<std::string>())["parents"] ==
                  Json::array({current}) &&
              text(read_bytes(path / "asset")) == "incoming",
          "resolved pick created incorrect ancestry or bytes");
  f.install(current);
  auto inverse = NativeResolutionSet::parse(
      f.store.plan_replay(path, "main", source, NativeReplayKind::revert)["resolution_context"]);
  require(inverse.base == source && inverse.parent == base,
          "revert resolution baseline or selected parent differs");
  inverse.files["asset"] = {NativeResolutionChoice::incoming};
  f.store.replay_directory_resolved(path, "main", current, source, NativeReplayKind::revert,
                                    "resolved inverse", inverse);
  require(text(read_bytes(path / "asset")) == "base", "resolved revert did not choose the parent version");
}
void mainline_context(const fs::path &path) {
  Fixture f(path);
  const auto left = f.revision(Json{{"asset", f.file("left parent")}});
  const auto right = f.revision(Json{{"asset", f.file("right parent")}});
  const auto source = f.revision(Json{{"asset", f.file("merged")}}, Json::array({left, right}));
  const auto current = f.revision(Json{{"asset", f.file("later")}}, Json::array({source}));
  f.install(current);
  auto set = NativeResolutionSet::parse(
      f.store.plan_replay(path, "main", source, NativeReplayKind::revert, 1)["resolution_context"]);
  set.files["asset"] = {NativeResolutionChoice::incoming};
  const auto before = f.physical();
  fails([&] { f.store.plan_replay_resolved(path, "main", source, NativeReplayKind::revert, set, 2); });
  fails([&] {
    f.store.replay_directory_resolved(path, "main", current, source, NativeReplayKind::revert,
                                      "wrong mainline", set, 2);
  });
  require(f.physical() == before, "changed mainline resolution mutated state");
  f.store.replay_directory_resolved(path, "main", current, source, NativeReplayKind::revert,
                                    "selected mainline", set, 1);
  require(text(read_bytes(path / "asset")) == "left parent", "resolved merge revert chose another parent");
}
void namespace_choices(const fs::path &path) {
  Fixture f(path);
  const auto a = f.file("ours"), b = f.file("incoming");
  const auto base = f.revision(Json::object());
  const auto current = f.revision(Json{{"Name", a}}, Json::array({base}));
  const auto source = f.revision(Json{{"name", b}}, Json::array({base}));
  f.install(current);
  auto set = NativeResolutionSet::parse(f.store.plan_merge(path, "main", source)["resolution_context"]);
  set.files["name"] = {NativeResolutionChoice::current};
  NativeMergeOptions settings;
  settings.resolutions = set;
  require(f.store.plan_merge(path, "main", source, settings)["ready"] == true,
          "namespace conflict could not discard the incoming alias");
  settings.resolutions->files["name"] = {NativeResolutionChoice::incoming};
  const auto still_conflicting = f.store.plan_merge(path, "main", source, settings);
  require(still_conflicting["ready"] == false && !still_conflicting["conflicts"].empty(),
          "explicit selection bypassed namespace collision checks");
  const auto left = f.revision(Json{{"a", a}, {"b", a}}, Json::array({base}));
  const auto right = f.revision(Json{{"a", b}, {"b", b}}, Json::array({base}));
  f.install(left);
  settings.resolutions =
      NativeResolutionSet::parse(f.store.plan_merge(path, "main", right)["resolution_context"]);
  settings.resolutions->files["a"] = {NativeResolutionChoice::incoming};
  const auto before = f.physical();
  require(f.store.plan_merge(path, "main", right, settings)["conflicts"].size() == 1,
          "partial resolution hid another conflict");
  fails([&] { f.store.merge_directory(path, "main", left, right, "partial", settings); });
  require(f.physical() == before, "partial resolution published compatible paths");
}
void missing_custom_payload(const fs::path &path) {
  std::string current, source, chunk;
  NativeMergeOptions settings;
  {
    Fixture f(path);
    const auto base = f.revision(Json{{"asset", f.file("base")}});
    current = f.revision(Json{{"asset", f.file("ours")}}, Json::array({base}));
    source = f.revision(Json{{"asset", f.file("incoming")}}, Json::array({base}));
    const auto custom = f.file("custom payload that will disappear");
    f.install(current);
    settings.resolutions =
        NativeResolutionSet::parse(f.store.plan_merge(path, "main", source)["resolution_context"]);
    const auto manifest = custom["manifest"].get<std::string>();
    settings.resolutions->files["asset"] = {NativeResolutionChoice::file, manifest, false};
    chunk = f.store.metadata(Kind::manifest, manifest)["blocks"][0]["id"].get<std::string>();
  }
  require(fs::remove(path / ".syrnck/objects/chunk" / chunk.substr(0, 2) / chunk),
          "custom missing block fixture failed");
  Store store(path / ".syrnck");
  const auto refs = store.refs();
  require(store.plan_merge(path, "main", source, settings)["ready"] == true,
          "metadata-only custom plan read payloads");
  const auto before = store.objects();
  fails([&] { store.plan_merge(path, "main", source, settings, true); });
  fails([&] { store.merge_directory(path, "main", current, source, "missing custom", settings); });
  require(store.refs() == refs && store.objects() == before, "broken custom resolution published state");
}
} // namespace
int main() {
  Temporary temp;
  try {
    schema();
    for (bool encrypted : {false, true})
      for (unsigned physical = 0; physical != 3; ++physical) {
        std::optional<Key> key;
        if (encrypted) {
          key.emplace();
          key->fill(61);
        }
        choices(temp.path / (std::to_string(encrypted) + '-' + std::to_string(physical)), key, physical);
      }
    namespace_choices(temp.path / "namespace");
    mainline_context(temp.path / "mainline");
    missing_custom_payload(temp.path / "missing");
    std::cout
        << "context-bound resolutions, all choices, schema, namespace, replay, verification and GC passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
