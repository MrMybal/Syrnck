#include "internal.hpp"
#include <iostream>
#include <sstream>
using namespace syrnck;
namespace {
template <class F> void fails(F function) {
  bool failed = false;
  try {
    function();
  } catch (const Error &) {
    failed = true;
  }
  require(failed, "expected native tag failure");
}
std::string revision(const Store &store, char value) {
  std::istringstream input(std::string(1024 * 1024 + 3, value));
  const auto manifest = store.ingest(input).first;
  return store
      .put(Kind::revision,
           bytes(Json{{"version", 1},
                      {"parents", Json::array()},
                      {"message", "tag fixture"},
                      {"timestamp", 0},
                      {"files", Json{{"asset.bin", Json{{"manifest", manifest}, {"executable", false}}}}}}
                     .dump()))
      .first.id;
}
template <class F> void cancelled(F function, const Store &store) {
  const auto before = store.refs();
  std::stop_source stop;
  fails([&] {
    function(OperationOptions{stop.get_token(), [&](const ProgressEvent &event) {
                                if (event.phase == ProgressPhase::publishing)
                                  stop.request_stop();
                              }});
  });
  require(stop.stop_requested() && store.refs() == before, "cancelled tag operation published references");
}
void scenario(const fs::path &root, std::optional<Key> key, unsigned layout) {
  std::string first, second, archive_id;
  {
    Store store(root, key, true);
    first = revision(store, 'a');
    second = revision(store, 'b');
    store.set_ref("heads/main", {Kind::revision, first});
    if (layout == 1)
      store.pack();
    if (layout == 2)
      store.pack({512 * 1024 * 1024, 3});
    cancelled(
        [&](OperationOptions options) {
          store.create_native_tag("release/v1", first, "Version française", options);
        },
        store);
    store.create_native_tag("release/v1", first, "Version française");
    store.create_native_tag("light", second);
    store.create_native_tag("empty-note", first, "");
    const auto tag = store.native_tag("release/v1")["tag"];
    require(tag["message"] == "Version française" && tag["revision"] == first &&
                !tag["annotation"].is_null() && tag["timestamp"].is_number_unsigned() &&
                store.native_tag("light")["tag"]["message"].is_null() &&
                store.native_tag("empty-note")["tag"]["message"] == "",
            "tag annotation differs");
    const auto before = store.refs();
    fails([&] { store.create_native_tag("release/v1", second); });
    fails([&] { store.delete_native_tag("release/v1", second); });
    fails([&] { store.create_native_tag("big", first, std::string(65537, 'x')); });
    for (const auto &name : {"", "a/../b", "bad:name", "-force", "a//b"})
      fails([&] { store.create_native_tag(name, first); });
    require(store.refs() == before, "invalid tag request changed refs");
    store.create_native_tag(std::string(187, 'n'), first);
    fails([&] { store.create_native_tag(std::string(188, 'n'), first); });
    const auto snapshot = store.refs();
    const auto objects = store.objects();
    require(store.native_tags()["tags"].size() == 4 && store.resolve_native_revision("HEAD") == first &&
                store.resolve_native_revision("main") == first &&
                store.resolve_native_revision("tags/release/v1") == first &&
                store.resolve_native_revision("release/v1") == first &&
                store.resolve_native_revision(second) == second,
            "native revision resolution differs");
    store.native_tag("release/v1", true);
    require(store.refs() == snapshot && store.objects() == objects, "tag inspection wrote Store data");
    store.set_ref("heads/light", {Kind::revision, second});
    fails([&] { store.resolve_native_revision("light"); });
    require(store.resolve_native_revision("heads/light") == second &&
                store.resolve_native_revision("tags/light") == second,
            "explicit revision namespace did not resolve ambiguity");
    store.create_native_tag("HEAD", second);
    store.create_native_tag(std::string(64, 'f'), first);
    require(store.resolve_native_revision("HEAD") == first &&
                store.resolve_native_revision("tags/HEAD") == second &&
                store.resolve_native_revision("tags/" + std::string(64, 'f')) == first,
            "reserved selector handling differs");
    fails([&] { store.resolve_native_revision(std::string(64, 'f')); });
    const auto note = store.refs().at("tag-metadata/release/v1");
    store.set_ref("tags/release/v1", {Kind::revision, second});
    fails([&] { store.native_tag("release/v1"); });
    fails([&] { store.resolve_native_revision("release/v1"); });
    store.remove_ref("tags/release/v1");
    fails([&] { store.native_tags(); });
    fails([&] { store.create_native_tag("release/v1", first); });
    store.set_ref("tags/release/v1", {Kind::revision, first});
    auto annotation = parse_json(store.get(note));
    annotation["timestamp"] = -1;
    store.set_ref("tag-metadata/release/v1", store.put(Kind::chunk, bytes(annotation.dump())).first);
    fails([&] { store.native_tag("release/v1"); });
    store.set_ref("tag-metadata/release/v1", note);
    store.set_ref("tags/invalid", note);
    store.set_ref("heads/not-revision", note);
    fails([&] { store.native_tags(); });
    fails([&] { store.resolve_native_revision("heads/not-revision"); });
    store.remove_ref("tags/invalid");
    store.remove_ref("heads/not-revision");
    cancelled([&](OperationOptions options) { store.delete_native_tag("release/v1", first, options); },
              store);
    const auto removed = store.delete_native_tag("release/v1", first);
    archive_id = removed["archive"].get<std::string>();
    require(!store.refs().contains("tags/release/v1") && !store.refs().contains("tag-metadata/release/v1") &&
                removed["tag"] == tag && store.native_tag_archive(archive_id, true)["tag"] == tag,
            "tag deletion did not retain its version and annotation");
    const auto prefix = "archive/tags/" + archive_id;
    store.set_ref(prefix + "/target", {Kind::revision, second});
    fails([&] { store.native_tag_archive(archive_id); });
    fails([&] { store.restore_native_tag(archive_id); });
    store.set_ref(prefix + "/target", {Kind::revision, first});
    cancelled([&](OperationOptions options) { store.restore_native_tag(archive_id, options); }, store);
    // Only this archive's roots retain the old file now; other labels/branches
    // are deliberately removed from this unique fixture before destructive GC.
    for (const auto &[name, value] : store.refs())
      if (!name.starts_with(prefix + '/'))
        store.remove_ref(name);
    store.gc(true);
    fails([&] { store.metadata(Kind::revision, second); });
    const auto manifest =
        store.metadata(Kind::revision, first)["files"]["asset.bin"]["manifest"].get<std::string>();
    std::ostringstream restored;
    store.reconstruct(manifest, restored);
    require(restored.str() == std::string(1024 * 1024 + 3, 'a'), "archive-only GC lost file bytes");
  }
  {
    Store store(root, key);
    const auto restored = store.restore_native_tag(archive_id);
    require(restored["tag"]["name"] == "release/v1" && restored["tag"]["message"] == "Version française" &&
                restored["tag"]["revision"] == first,
            "archive restore after reopening differs");
    fails([&] { store.restore_native_tag(archive_id); });
    const auto light = store.create_native_tag("light", first);
    const auto removed = store.delete_native_tag("light", first);
    require(removed["tag"] == light["tag"] && removed["tag"]["annotation"].is_null(),
            "light tag archive differs");
    store.restore_native_tag(removed["archive"].get<std::string>());
    std::stop_source stopped;
    stopped.request_stop();
    fails([&] { store.native_tags({}, 100, {stopped.get_token()}); });
    require(store.fsck() > 0, "native tag audit failed");
  }
}
void pagination(const fs::path &root) {
  Store store(root, {}, true);
  const auto target = revision(store, 'p');
  const std::string message(65536, '\0');
  for (unsigned n = 0; n != 50; ++n)
    store.create_native_tag("label-" + std::to_string(n), target, message);
  const auto before = store.refs();
  const auto first = store.native_tags({}, 1000);
  require(first["has_more"] == true && first["tags"].size() > 0 && first["tags"].size() < 50 &&
              first.dump().size() <= max_object_size,
          "escaped tag messages exceeded the page budget");
  const auto next = store.native_tags(first["next_after"].get<std::string>(), 1000);
  require(next["has_more"] == false && next["tags"].size() + first["tags"].size() == 50 &&
              store.refs() == before,
          "tag pagination lost labels or changed references");
  fails([&] { store.native_tags({}, 0); });
  fails([&] { store.native_tags({}, 1001); });
}
void missing_payload(const fs::path &root) {
  Store store(root, {}, true);
  const auto target = revision(store, 'a');
  store.create_native_tag("kept", target, "note");
  const auto manifest =
      store.metadata(Kind::revision, target)["files"]["asset.bin"]["manifest"].get<std::string>();
  const auto block = store.metadata(Kind::manifest, manifest)["blocks"][0]["id"].get<std::string>();
  require(fs::remove(root / "objects/chunk" / block.substr(0, 2) / block), "cannot remove fixture chunk");
  const auto before = store.refs();
  require(store.native_tag("kept")["tag"]["revision"] == target &&
              store.resolve_native_revision("kept") == target,
          "metadata-only tag inspection failed");
  fails([&] { store.native_tag("kept", true); });
  fails([&] { store.create_native_tag("broken", target, "new note"); });
  fails([&] { store.delete_native_tag("kept", target); });
  require(store.refs() == before, "incomplete closure changed tag retention");
}
} // namespace
int main() {
  const auto root = fs::temp_directory_path() / ("syrnck-native-tags-" + hex(random_bytes(16)));
  struct Cleanup {
    fs::path root;
    ~Cleanup() {
      std::error_code error;
      fs::remove_all(root, error);
    }
  } cleanup{root};
  try {
    for (bool encrypted : {false, true})
      for (unsigned layout = 0; layout != 3; ++layout) {
        std::optional<Key> key;
        if (encrypted) {
          key.emplace();
          key->fill(37);
        }
        scenario(root / (std::to_string(encrypted) + '-' + std::to_string(layout)), key, layout);
      }
    missing_payload(root / "missing");
    pagination(root / "pagination");
    std::cout << "native tags, annotations, archives, cancellation, resolution and GC passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
