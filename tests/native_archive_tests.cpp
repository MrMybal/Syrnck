#include "native_history_fixture.hpp"
#include <iostream>
using namespace native_test;
namespace {
void inventory(const fs::path &path, std::optional<Key> key, unsigned physical) {
  Fixture f(path, key);
  const auto root = f.revision(Json{{"asset", f.file("retained")}});
  const auto head = f.revision(Json{{"asset", f.file("head")}}, Json::array({root}));
  f.install(head);
  f.store.amend_message("main", head, "Amended");
  const auto amended = f.store.refs().at("heads/main").id;
  f.store.squash_commits("main", amended, 2, "Combined");
  const auto squashed = f.store.refs().at("heads/main").id;
  f.write("asset", "prepared");
  const auto index = f.store.stage_directory(path, "main", {"asset"});
  f.store.reset_branch("main", squashed, root, NativeResetMode::mixed);
  f.store.set_ref("archive/" + root, {Kind::revision, root});
  f.store.set_ref("archive/checkout/" + std::string(64, 'a'), {Kind::revision, head});
  f.store.set_ref("archive/custom", {Kind::revision, root});
  f.store.create_native_tag("old", root);
  f.store.delete_native_tag("old", root);
  auto long_note = f.store.metadata(Kind::revision, head);
  long_note["message"] = std::string(255, 'x') + "\xc3\xa9" + std::string(700, 'y');
  const auto noted = f.store.put(Kind::revision, bytes(long_note.dump())).first;
  f.store.set_ref("archive/amend/" + noted.id, noted);
  f.pack(physical);
  const auto before = f.physical();
  const auto all = f.store.native_archives({}, {}, 100, true);
  require(all["archives"].size() == 7 && all["objects_verified"] == true && all["has_more"] == false &&
              all["next_after"].is_null(),
          "archive inventory adopted foreign/tag roots or lost history");
  std::optional<std::string> after;
  Json paged = Json::array();
  for (;;) {
    const auto page = f.store.native_archives({}, after, 1);
    require(page["archives"].size() == 1, "archive page did not advance");
    paged.push_back(page["archives"][0]);
    if (page["next_after"].is_null())
      break;
    after = page["next_after"].get<std::string>();
  }
  require(paged == all["archives"], "archive pages skipped or repeated roots");
  for (const auto &row : all["archives"])
    require(f.store.native_archive(row["reference"].get<std::string>(), true)["archive"] == row,
            "archive inspection differs from inventory");
  const auto note = f.store.native_archive("archive/amend/" + noted.id)["archive"];
  require(note["message"] == std::string(255, 'x') && note["message_truncated"] == true &&
              note["message_bytes"] == 957,
          "archive preview split a UTF-8 sequence");
  const auto staged = f.store.native_archives("reset-index");
  require(staged["archives"].size() == 1 && staged["archives"][0]["revision"] == index &&
              f.physical() == before,
          "archive inspection wrote data or lost prepared roots");
  fails([&] { f.store.native_archives("tags"); });
  fails([&] { f.store.native_archives("squash", "archive/reset/" + head); });
  fails([&] { f.store.native_archives({}, {}, 0); });
  fails([&] { f.store.native_archives({}, {}, 1001); });
  fails([&] { f.store.native_archive("archive/custom"); });
  fails([&] { f.store.native_archive("archive/reset/nested/" + head); });
  std::stop_source stop;
  fails([&] {
    f.store.native_archives({}, {}, 100, false,
                            {stop.get_token(), [&](const ProgressEvent &) { stop.request_stop(); }});
  });
  require(f.physical() == before, "cancelled archive inventory wrote data");
  f.store.gc(true);
  require(f.store.native_archives({}, {}, 100, true)["archives"] == all["archives"],
          "GC lost an archived revision");
  const auto wrong = "archive/reset/" + head;
  f.store.set_ref(wrong, {Kind::revision, root});
  fails([&] { f.store.native_archives("reset"); });
  fails([&] { f.store.native_archive(wrong); });
}
void missing_payload(const fs::path &path) {
  std::string id, chunk;
  {
    Fixture f(path);
    const auto file = f.file("archive payload");
    id = f.revision(Json{{"asset", file}});
    f.store.set_ref("archive/reset/" + id, {Kind::revision, id});
    chunk = f.store.metadata(Kind::manifest, file["manifest"].get<std::string>())["blocks"][0]["id"]
                .get<std::string>();
  }
  require(fs::remove(path / ".syrnck/objects/chunk" / chunk.substr(0, 2) / chunk),
          "missing archive fixture failed");
  Store store(path / ".syrnck");
  require(store.native_archives()["archives"].size() == 1, "metadata archive query read payloads");
  fails([&] { store.native_archive("archive/reset/" + id, true); });
  fails([&] { store.native_archives({}, {}, 100, true); });
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
          key->fill(49);
        }
        inventory(temp.path / (std::to_string(encrypted) + '-' + std::to_string(physical)), key, physical);
      }
    missing_payload(temp.path / "missing");
    std::cout << "archive categories, pagination, read-only verification, UTF-8 previews and GC passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
