#include "native_history_fixture.hpp"
#include <syrnck/history.hpp>
#include <iostream>
using namespace native_test;
namespace {
void comparison(const fs::path &root, std::optional<Key> key, unsigned layout) {
  Fixture f(root, key);
  const auto old = f.file(std::string(7000, 'x'), false, 1024),
             reencoded = f.file(std::string(7000, 'x'), true, 2048);
  const auto duplicate = f.file("duplicate"), stable = f.file("stable");
  const auto a =
      f.revision(Json{{"Old/Édit.uasset", old}, {"dup1", duplicate}, {"dup2", duplicate}, {"keep", stable}});
  const auto b = f.revision(
      Json{{"New/édit.uasset", reencoded}, {"dup3", duplicate}, {"added", f.file("added")}, {"keep", stable}},
      Json::array({a}));
  f.install(b);
  f.pack(layout);
  const auto before = f.physical();
  const auto plain = f.store.compare_revisions(a, b);
  const auto report = native_diff_renames(f.store, a, b);
  require(report["counts"]["renamed"] == 1 && report["counts"]["added"] == 2 &&
              report["counts"]["removed"] == 2,
          "exact rename comparison changed file accounting");
  require(plain["counts"]["added"] == 3 && plain["counts"]["removed"] == 3,
          "legacy diff changed rename semantics");
  require(report["rename_detection"]["ambiguous_sources"] == 2 &&
              report["rename_detection"]["ambiguous_targets"] == 1,
          "duplicate contents were guessed as a rename");
  bool found = false;
  for (const auto &row : report["changes"])
    if (row["status"] == "renamed") {
      require(row["old_path"] == "Old/Édit.uasset" && row["path"] == "New/édit.uasset" &&
                  row["similarity"] == 100 && row["executable_changed"] == true &&
                  row["manifest_changed"] == true && row["content_changed"] == false,
              "renamed file lost its representation or mode change");
      found = true;
    }
  require(found && f.physical() == before, "rename planning wrote storage");
  const auto ambiguous = native_file_history(f.store, b, "dup3");
  require(ambiguous["entries"].size() == 1 && ambiguous["stop_reason"] == "ambiguous_rename",
          "file history hid an ambiguous rename origin");
}
void history(const fs::path &root) {
  Fixture f(root);
  const auto stable = f.file("keep"), first = f.file("first"), second = f.file("second"),
             third = f.file("third");
  auto files = [&](std::string_view path, const Json &file) {
    return Json{{"keep", stable}, {std::string(path), file}};
  };
  const auto a = f.revision(files("old.cpp", first));
  const auto b = f.revision(files("old.cpp", second), Json::array({a}));
  const auto c = f.revision(files("middle.cpp", second), Json::array({b}));
  const auto d = f.revision(files("middle.cpp", second), Json::array({c}));
  const auto e = f.revision(files("new.cpp", second), Json::array({d}));
  const auto g = f.revision(files("new.cpp", third), Json::array({e}));
  const auto h = f.revision(Json{{"keep", stable}}, Json::array({g}));
  f.install(h);
  const auto before = f.physical();
  const auto report = native_file_history(f.store, h, "new.cpp");
  std::vector<std::string> ids;
  for (const auto &row : report["entries"])
    ids.push_back(row["revision"].get<std::string>());
  require(ids == std::vector<std::string>{h, g, e, c, b, a} && report["complete"] == true,
          "file history did not follow exact renames/deletions or skipped an edit");
  const auto page = native_file_history(f.store, h, "new.cpp", {2, 1000, true});
  require(page["next_revision"] == e && page["next_path"] == "new.cpp",
          "history cursor lost its revision/path context");
  const auto rest = native_file_history(f.store, page["next_revision"].get<std::string>(),
                                        page["next_path"].get<std::string>());
  require(rest["entries"].size() == 4 && rest["entries"][0]["revision"] == e,
          "history pagination duplicated or lost entries");
  const auto bounded = native_file_history(f.store, d, "middle.cpp", {100, 1, true});
  require(bounded["entries"].empty() && bounded["complete"] == false && bounded["next_revision"] == c,
          "history traversal limit silently hid continuation");
  const auto no_follow = native_file_history(f.store, g, "new.cpp", {100, 1000, false});
  require(no_follow["entries"].size() == 2 && no_follow["entries"][1]["change"]["status"] == "added",
          "disabled rename following crossed an addition boundary");
  const auto away = native_file_history(f.store, e, "middle.cpp");
  require(away["entries"][0]["revision"] == e && away["entries"][0]["change"]["status"] == "renamed",
          "history omitted the rename away from the requested path");
  fails([&] { native_file_history(f.store, h, "new.cpp", {0, 1000, true}); });
  std::stop_source stop;
  stop.request_stop();
  fails([&] { native_file_history(f.store, h, "new.cpp", {}, {stop.get_token()}); });
  require(f.physical() == before, "read-only file history wrote objects");
}
void metadata_only(const fs::path &root) {
  Fixture f(root);
  const auto file = f.file("missing payload");
  const auto a = f.revision(Json{{"old", file}}), b = f.revision(Json{{"new", file}}, Json::array({a}));
  const auto metadata = f.store.metadata(Kind::manifest, file["manifest"].get<std::string>());
  const auto id = metadata["blocks"][0]["id"].get<std::string>();
  fs::remove(f.store.root() / "objects/chunk" / id.substr(0, 2) / id);
  f.store.clear_cache();
  const auto before = f.physical();
  require(native_diff_renames(f.store, a, b)["counts"]["renamed"] == 1,
          "rename metadata required payload reads");
  require(native_file_history(f.store, b, "new")["entries"].size() == 2,
          "file history required payload reads");
  require(f.physical() == before, "metadata-only history changed storage");
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
          key->fill(72);
        }
        comparison(temporary.path / (std::to_string(encrypted) + '-' + std::to_string(layout)), key, layout);
      }
    history(temporary.path / "history");
    metadata_only(temporary.path / "metadata");
    std::cout << "Exact rename comparison, conservative duplicates and paged first-parent file history\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
