#include "native_history_fixture.hpp"
#include <iostream>
using namespace native_test;
namespace {
void run(const fs::path &root, std::optional<Key> key, unsigned layout) {
  Fixture f(root, key);
  const std::string path = "Source/Edited source.cpp";
  const auto original_file = f.file("one\r\ntwo\r\nlast");
  const auto our_file = f.file("ONE\r\ntwo\r\nlast", true);
  const auto their_file = f.file("one\r\ntwo\r\nLAST");
  const auto ancestor = f.revision(Json{{"ancestor.bin", f.file("not requested either")}});
  const auto base = f.revision(Json{{path, original_file}, {"unrelated.bin", f.file("not requested")}},
                               Json::array({ancestor}));
  const auto ours = f.revision(Json{{path, our_file}}, Json::array({base}));
  const auto theirs = f.revision(Json{{path, their_file}}, Json::array({base}));
  const auto changed = f.revision(Json{{path, f.file("OTHER\r\ntwo\r\nlast")}});
  const auto binary = f.revision(Json{{path, f.file(std::string("nul\0", 4))}});
  const auto absent = f.revision(Json::object());
  const auto empty = f.revision(Json{{path, f.file("")}});
  const auto representation = f.revision(Json{{path, f.file("one\r\ntwo\r\nlast", true, 2048)}});
  f.store.set_ref("heads/main", {Kind::revision, ours});
  f.write(path_from_utf8(path), "working bytes must not be read");
  f.pack(layout);
  const auto before = f.physical();
  const auto result = f.store.merge_revision_file(base, ours, theirs, path);
  require(result["ready"] == true && result["objects_verified"] == true &&
              result["merge"]["content"] == "ONE\r\ntwo\r\nLAST" && result["result_executable"] == true &&
              result["base"]["manifest"] == original_file["manifest"] && result["blockers"].empty(),
          "stored text merge differs");
  const auto mode = f.store.merge_revision_file(base, base, representation, path);
  require(mode["ready"] == true && mode["result_executable"] == true &&
              mode["merge"]["content"] == "one\r\ntwo\r\nlast",
          "merge lost mode or logical equality");
  const auto conflict = f.store.merge_revision_file(base, ours, changed, path);
  require(conflict["ready"] == false && conflict["objects_verified"] == true &&
              conflict["merge"]["status"] == "conflict" && conflict["merge"]["content"].is_null() &&
              conflict["result_executable"].is_null(),
          "stored conflict leaked content");
  const auto non_text = f.store.merge_revision_file(base, ours, binary, path);
  require(non_text["ready"] == false && non_text["merge"]["status"] == "binary", "stored binary merged");
  const auto missing = f.store.merge_revision_file(base, ours, absent, path);
  require(missing["ready"] == false && missing["merge"].is_null() &&
              missing["blockers"] == Json::array({"file_presence"}) && missing["objects_verified"] == false,
          "presence change silently treated as an empty file");
  require(f.store.merge_revision_file(absent, absent, absent, path)["ready"] == false, "absent path merged");
  require(f.store.merge_revision_file(empty, empty, empty, path)["merge"]["content"] == "",
          "empty existing files lost presence");
  TextDiffOptions limits;
  limits.max_work = 1;
  require(f.store.merge_revision_file(base, ours, theirs, path, limits)["merge"]["status"] == "limited",
          "stored merge ignored shared work budget");
  fails([&] { f.store.merge_revision_file(base, ours, theirs, "../unsafe"); });
  fails([&] { f.store.merge_revision_file("invalid", ours, theirs, path); });
  std::stop_source stop;
  fails([&] {
    f.store.merge_revision_file(base, ours, theirs, path, {},
                                {stop.get_token(), [&](const ProgressEvent &event) {
                                   if (event.phase == ProgressPhase::reconstructing && event.bytes)
                                     stop.request_stop();
                                 }});
  });
  fails([&] {
    f.store.merge_revision_file(base, ours, theirs, path, {},
                                {{}, [](const ProgressEvent &event) {
                                   if (event.complete && event.phase == ProgressPhase::planning)
                                     throw Error("merge observer sentinel");
                                 }});
  });
  require(f.physical() == before, "text merge preview changed objects, refs or working files");

  Store partial(root / "partial", key, true);
  // Metadata-only limits must run before any payload reads. The unrelated file
  // and parent snapshots also remain absent throughout the targeted operation.
  for (const auto &id : {base, ours, theirs}) {
    const ObjectKey revision{Kind::revision, id};
    partial.import(revision, f.store.get(revision));
  }
  for (const auto &entry : {original_file, our_file, their_file}) {
    const ObjectKey manifest{Kind::manifest, entry["manifest"].get<std::string>()};
    partial.import(manifest, f.store.get(manifest));
  }
  limits = {};
  limits.max_file_bytes = 1;
  const auto bounded = partial.merge_revision_file(base, ours, theirs, path, limits);
  require(bounded["merge"].is_null() && bounded["objects_verified"] == false &&
              bounded["blockers"] == Json::array({"file_limit"}),
          "file limit loaded absent chunks");
  fails([&] { partial.merge_revision_file(base, ours, theirs, path); });
  for (const auto &entry : {original_file, our_file, their_file}) {
    const auto manifest = f.store.metadata(Kind::manifest, entry["manifest"].get<std::string>());
    for (const auto &block : manifest["blocks"]) {
      const ObjectKey chunk{Kind::chunk, block["id"].get<std::string>()};
      partial.import(chunk, f.store.get(chunk));
    }
  }
  require(partial.merge_revision_file(base, ours, theirs, path) == result,
          "targeted merge traversed unrelated payloads");
  require(!partial.contains({Kind::revision, ancestor}), "fixture unexpectedly imported an ancestor");
  auto bad_manifest = f.store.metadata(Kind::manifest, their_file["manifest"].get<std::string>());
  bad_manifest["file_hash"] = std::string(64, '0');
  const auto bad = f.store.put(Kind::manifest, bytes(bad_manifest.dump())).first;
  const auto corrupt = f.revision(Json{{path, Json{{"manifest", bad.id}, {"executable", false}}}});
  fails([&] { f.store.merge_revision_file(base, ours, corrupt, path); });
}
} // namespace
int main() {
  Temporary temporary;
  try {
    for (bool encrypted : {false, true})
      for (unsigned layout = 0; layout < 3; ++layout) {
        std::optional<Key> key;
        if (encrypted) {
          key.emplace();
          key->fill(47);
        }
        run(temporary.path / (std::to_string(encrypted) + '-' + std::to_string(layout)), key, layout);
      }
    std::cout << "Verified targeted three-snapshot text merge, modes, integrity, budgets and read-only "
                 "storage variants\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
