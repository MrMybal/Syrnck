#include "internal.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
using namespace syrnck;
namespace {
template <class F> void fails(F function, std::string_view expected = {}) {
  bool failed = false;
  try {
    function();
  } catch (const Error &error) {
    failed = expected.empty() || error.what() == expected;
  }
  require(failed, "expected native comparison failure");
}
void write(const fs::path &path, std::string_view value) {
  fs::create_directories(path.parent_path());
  atomic_write(path, bytes(value), true);
}
std::map<std::string, std::string> physical(const fs::path &root) {
  std::map<std::string, std::string> result;
  for (const auto &entry : fs::recursive_directory_iterator(root))
    if (entry.is_regular_file() && entry.path().filename() != "store.lock")
      result.emplace(utf8(entry.path().lexically_relative(root)), hash(read_bytes(entry.path())));
  return result;
}
std::map<std::string, Json> rows(const Json &report) {
  std::map<std::string, Json> result;
  for (const auto &row : report["changes"])
    result.emplace(row["path"].get<std::string>(), row);
  return result;
}
} // namespace
int main() {
  auto root = fs::temp_directory_path() / ("syrnck-native-compare-" + hex(random_bytes(16)));
  struct Cleanup {
    fs::path root;
    ~Cleanup() {
      std::error_code error;
      fs::remove_all(root, error);
    }
  } cleanup{root};
  try {
    for (bool encrypted : {false, true}) {
      for (unsigned layout = 0; layout != 3; ++layout) {
        const auto project = root / (std::to_string(encrypted) + '-' + std::to_string(layout));
        std::optional<Key> key;
        if (encrypted) {
          key.emplace();
          key->fill(47);
        }
        Store store(project / ".syrnck", key, true);
        write(project / ".syrnckignore", "Ignored/\n*.tmp\n!important.tmp\n");
        write(project / "changed.bin", std::string(3 * 1024 * 1024 + 5, 'a'));
        write(project / "removed.txt", "removed later");
        write(project / "same.txt", "same content across chunking policies");
        write(project / "empty.bin", "");
        write(project / path_from_utf8("Éditeur.cpp"), "source code\n");
        write(project / "important.tmp", "included");
        write(project / "Ignored/cache", "ignored");
        write(project / "temporary.tmp", "ignored");
        write(project / ".git/fake", "excluded Git data");
        const auto first = store.commit_directory(project, "main", "baseline");
        if (layout == 1)
          store.pack();
        if (layout == 2)
          store.pack({512 * 1024 * 1024, 2});
        const auto original = physical(store.root());
        const auto clean = store.status_directory(project, first);
        require(clean["clean"] == true && clean["counts"]["unchanged"] == 7 && clean["changes"].empty(),
                "clean working tree differs");
        const auto unborn = store.status_directory(project);
        require(unborn["base"].is_null() && unborn["counts"]["untracked"] == 7, "empty baseline differs");
        require(physical(store.root()) == original, "status wrote Store data");

        const auto changed = project / "changed.bin";
        const auto modified = fs::last_write_time(changed);
        write(changed, std::string(3 * 1024 * 1024 + 5, 'b'));
        fs::last_write_time(changed, modified);
        fs::remove(project / "removed.txt");
        write(project / "new.txt", "new content");
        const auto status = store.status_directory(project, first);
        require(status["clean"] == false &&
                    status["counts"] ==
                        Json{{"untracked", 1}, {"removed", 1}, {"modified", 1}, {"unchanged", 5}},
                "working tree changes differ");
        auto changed_rows = rows(status);
        require(changed_rows.size() == 3 && changed_rows.at("changed.bin")["content_changed"] == true &&
                    changed_rows.at("changed.bin")["source"]["size"] ==
                        changed_rows.at("changed.bin")["target"]["size"] &&
                    changed_rows.at("new.txt")["source"].is_null() &&
                    changed_rows.at("new.txt")["target"]["manifest"].is_null() &&
                    changed_rows.at("removed.txt")["target"].is_null(),
                "working file summaries differ");
        require(physical(store.root()) == original, "dirty status wrote Store data");
        const auto caller = std::this_thread::get_id();
        std::stop_source stopped;
        fails(
            [&] {
              store.status_directory(project, first,
                                     {stopped.get_token(), [&](const ProgressEvent &event) {
                                        require(caller == std::this_thread::get_id(),
                                                "native observer changed thread");
                                        if (event.phase == ProgressPhase::verifying && event.bytes > 0)
                                          stopped.request_stop();
                                      }});
            },
            "operation cancelled");
        fails([&] { store.compare_revisions(first, first, {stopped.get_token()}); }, "operation cancelled");
        fails(
            [&] {
              store.status_directory(project, first, {{}, [](const ProgressEvent &event) {
                                                        if (event.phase == ProgressPhase::verifying)
                                                          throw Error("native observer sentinel");
                                                      }});
            },
            "native observer sentinel");
        bool grew = false;
        fails(
            [&] {
              store.status_directory(project, first,
                                     {{}, [&](const ProgressEvent &event) {
                                        if (!grew && event.phase == ProgressPhase::verifying &&
                                            event.path == "changed.bin") {
                                          grew = true;
                                          std::ofstream out(changed, std::ios::binary | std::ios::app);
                                          out.put('!');
                                          require(out.good(), "cannot mutate native test source");
                                        }
                                      }});
            },
            "file changed during native scan; retry operation");
        require(grew && physical(store.root()) == original, "failed scan wrote Store data");

        const auto second = store.commit_directory(project, "main", "changed snapshot");
        const auto comparison = store.compare_revisions(first, second);
        require(comparison["equal"] == false &&
                    comparison["counts"] ==
                        Json{{"added", 1}, {"removed", 1}, {"modified", 1}, {"unchanged", 5}},
                "revision changes differ");
        require(store.compare_revisions(second, second)["equal"] == true,
                "identical snapshot comparison differs");
        auto description = store.metadata(Kind::revision, second);
        description["message"] = "same files with different revision metadata";
        const auto described = store.put(Kind::revision, bytes(description.dump())).first;
        require(store.compare_revisions(second, described.id)["equal"] == true,
                "revision message changed file comparison");
        auto revision = store.metadata(Kind::revision, second);
        std::istringstream same("same content across chunking policies");
        const auto alternate = store.ingest(same, Chunking::fixed_size(1024)).first;
        revision["files"]["same.txt"]["manifest"] = alternate;
        revision["files"]["Éditeur.cpp"]["executable"] = true;
        revision["parents"] = Json::array({second});
        revision["message"] = "representation and mode changes";
        const auto third = store.put(Kind::revision, bytes(revision.dump())).first;
        if (layout != 0)
          store.pack(layout == 2 ? PackOptions{512 * 1024 * 1024, 2}
                                 : PackOptions{512 * 1024 * 1024, 65536, true});
        const auto before = physical(store.root());
        const auto metadata_changes = store.compare_revisions(second, third.id);
        changed_rows = rows(metadata_changes);
        require(changed_rows.size() == 2 && changed_rows.at("same.txt")["content_changed"] == false &&
                    changed_rows.at("same.txt")["manifest_changed"] == true &&
                    changed_rows.at("Éditeur.cpp")["executable_changed"] == true &&
                    changed_rows.at("Éditeur.cpp")["manifest_changed"] == false,
                "representation or mode changes differ");
        require(physical(store.root()) == before, "revision comparison wrote Store data");

        // A partial metadata-only Store deliberately lacks chunks and parents.
        Store partial(root / ("partial-" + std::to_string(encrypted) + '-' + std::to_string(layout)), key,
                      true);
        partial.import(third, store.get(third));
        for (const auto &[path, entry] : revision["files"].items()) {
          (void)path;
          ObjectKey manifest{Kind::manifest, entry["manifest"].get<std::string>()};
          partial.import(manifest, store.get(manifest));
        }
        require(!partial.contains({Kind::revision, second}) &&
                    partial.compare_revisions(third.id, third.id)["equal"] == true,
                "comparison traversed snapshot parents");
        const ObjectKey base{Kind::revision, second};
        partial.import(base, store.get(base));
        const auto prior = store.metadata(Kind::revision, second);
        for (const auto &[path, entry] : prior["files"].items()) {
          (void)path;
          const ObjectKey manifest{Kind::manifest, entry["manifest"].get<std::string>()};
          partial.import(manifest, store.get(manifest));
        }
        require(partial.compare_revisions(second, third.id) == metadata_changes,
                "comparison required chunk payloads");
        auto broken = revision;
        broken["files"]["same.txt"]["manifest"] = std::string(64, '0');
        const auto invalid = partial.put(Kind::revision, bytes(broken.dump())).first;
        fails([&] { partial.compare_revisions(third.id, invalid.id); });
        std::stop_source planning_stop;
        fails(
            [&] {
              store.compare_revisions(first, second, {planning_stop.get_token(), [&](const ProgressEvent &) {
                                                        planning_stop.request_stop();
                                                      }});
            },
            "operation cancelled");
        require(store.compare_revisions(second, third.id) == metadata_changes,
                "comparison did not recover after cancellation");
      }
    }
    std::cout << "Verified native status/diff, content hashes, metadata-only reads, storage layouts and "
                 "cancellation\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
