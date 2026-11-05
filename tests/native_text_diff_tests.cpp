#include "internal.hpp"
#include <fstream>
#include <iostream>
#include <sstream>
using namespace syrnck;
namespace {
template <class F> void fails(F fn, std::string_view expected = {}) {
  try {
    fn();
  } catch (const Error &e) {
    require(expected.empty() || std::string_view(e.what()) == expected, "unexpected native text diff error");
    return;
  }
  throw Error("expected native text diff failure");
}
std::map<std::string, std::string> physical(const fs::path &root) {
  std::map<std::string, std::string> result;
  for (const auto &entry : fs::recursive_directory_iterator(root))
    if (entry.is_regular_file() && entry.path().filename() != "store.lock")
      result.emplace(utf8(entry.path().lexically_relative(root)), hash(read_bytes(entry.path())));
  return result;
}
std::string ingest(const Store &store, std::string_view content, std::uint32_t block = 1024) {
  std::istringstream input{std::string(content)};
  return store.ingest(input, Chunking::fixed_size(block)).first;
}
Json file(std::string manifest, bool executable = false) {
  return Json{{"manifest", std::move(manifest)}, {"executable", executable}};
}
ObjectKey revision(const Store &store, Json files, std::optional<std::string> parent = {}) {
  std::map<std::string, Json> sorted;
  for (const auto &[path, entry] : files.items())
    sorted.emplace(path, entry);
  files = Json::object();
  for (const auto &[path, entry] : sorted)
    files[path] = entry;
  const auto value = Json{{"version", 1},
                          {"parents", parent ? Json::array({*parent}) : Json::array()},
                          {"message", "text diff fixture"},
                          {"timestamp", 0},
                          {"files", std::move(files)}};
  return store.put(Kind::revision, bytes(value.dump())).first;
}
} // namespace
int main() {
  const auto root = fs::temp_directory_path() / ("syrnck-native-text-" + hex(random_bytes(16)));
  struct Cleanup {
    fs::path root;
    ~Cleanup() {
      std::error_code ec;
      fs::remove_all(root, ec);
    }
  } cleanup{root};
  try {
    for (bool encrypted : {false, true})
      for (unsigned layout = 0; layout < 3; ++layout) {
        std::optional<Key> key;
        if (encrypted) {
          key.emplace();
          key->fill(73);
        }
        Store store(root / (std::to_string(encrypted) + '-' + std::to_string(layout)), key, true);
        const auto a = ingest(store, "one\ntwo\nthree");
        const auto b = ingest(store, "one\r\ntwo changed\r\nthree\r\n");
        const auto empty = ingest(store, "");
        const auto binary = ingest(store, std::string("binary\0data", 11));
        const auto old = revision(store, Json{{"src/Édit.cpp", file(a)},
                                              {"removed", file(a)},
                                              {"empty", file(empty)},
                                              {"asset.bin", file(binary)}});
        const auto next = revision(store,
                                   Json{{"src/Édit.cpp", file(b, true)},
                                        {"added", file(a)},
                                        {"empty", file(empty)},
                                        {"asset.bin", file(binary)}},
                                   old.id);
        store.set_ref("heads/main", next);
        if (layout)
          store.pack(layout == 1 ? PackOptions{} : PackOptions{512 * 1024 * 1024, 2});
        const auto before = physical(store.root());
        const auto diff = store.diff_revision_file(old.id, next.id, "src/Édit.cpp");
        require(diff["content_changed"] == true && diff["executable_changed"] == true &&
                    diff["objects_verified"] == true && diff["diff"]["status"] == "text" &&
                    diff["diff"]["deletions"] == 3 && diff["diff"]["additions"] == 3,
                "native text diff differs");
        require(store.diff_revision_file(old.id, next.id, "removed")["target"].is_null(),
                "removed file differs");
        require(store.diff_revision_file(old.id, next.id, "added")["source"].is_null(), "added file differs");
        require(store.diff_revision_file(old.id, next.id, "empty")["equal"] == true, "empty file differs");
        const auto added_empty = store.diff_revision_file({}, next.id, "empty");
        require(added_empty["equal"] == false && added_empty["diff"]["equal"] == true,
                "empty addition lost presence");
        require(store.diff_revision_file(old.id, next.id, "asset.bin")["diff"]["status"] == "binary",
                "binary file emitted text");
        fails([&] { store.diff_revision_file(old.id, next.id, "absent"); },
              "native text diff path is absent from both snapshots");
        fails([&] { store.diff_revision_file(old.id, next.id, "../unsafe"); });
        std::stop_source stopped;
        fails(
            [&] {
              store.diff_revision_file(old.id, next.id, "src/Édit.cpp", {},
                                       {stopped.get_token(), [&](const ProgressEvent &e) {
                                          if (e.phase == ProgressPhase::reconstructing && e.bytes > 0)
                                            stopped.request_stop();
                                        }});
            },
            "operation cancelled");
        require(physical(store.root()) == before, "native text diff changed the Store");
        Store partial(root / ("partial-" + std::to_string(encrypted) + '-' + std::to_string(layout)), key,
                      true);
        partial.import(next, store.get(next));
        const ObjectKey manifest{Kind::manifest, b};
        partial.import(manifest, store.get(manifest));
        TextDiffOptions small;
        small.max_file_bytes = 1;
        const auto metadata_only = partial.diff_revision_file({}, next.id, "src/Édit.cpp", small);
        require(metadata_only["objects_verified"] == false && metadata_only["diff"]["reason"] == "file_limit",
                "oversized diff tried to reconstruct chunks");
        fails([&] { partial.diff_revision_file({}, next.id, "src/Édit.cpp"); });
        const auto complete_manifest = store.metadata(Kind::manifest, b);
        for (const auto &entry : complete_manifest["blocks"]) {
          const ObjectKey chunk{Kind::chunk, entry["id"].get<std::string>()};
          partial.import(chunk, store.get(chunk));
        }
        require(partial.diff_revision_file({}, next.id, "src/Édit.cpp")["objects_verified"] == true &&
                    !partial.contains(old),
                "targeted diff read unrelated files or parents");
        auto bad_manifest = store.metadata(Kind::manifest, b);
        bad_manifest["file_hash"] = std::string(64, '0');
        const auto bad = store.put(Kind::manifest, bytes(bad_manifest.dump())).first;
        const auto corrupt = revision(store, Json{{"src/Édit.cpp", file(bad.id)}});
        fails([&] { store.diff_revision_file(old.id, corrupt.id, "src/Édit.cpp"); },
              "reconstructed file hash mismatch");
        const auto representation = ingest(store, "one\ntwo\nthree", 2048);
        const auto mode = revision(store, Json{{"src/Édit.cpp", file(representation, true)}});
        const auto logical = store.diff_revision_file(old.id, mode.id, "src/Édit.cpp");
        require(logical["content_changed"] == false && logical["executable_changed"] == true &&
                    logical["manifest_changed"] == true && logical["diff"]["hunks"].empty(),
                "mode/representation lost");
      }
    std::cout << "Verified targeted revision text diffs, all storage variants, payload integrity and "
                 "read-only behavior\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
