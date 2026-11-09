#include "native_history_fixture.hpp"
#include "native_renames.hpp"
#include <syrnck/history.hpp>
#include <iostream>
using namespace native_test;
namespace {
std::string source_text() {
  std::string text;
  for (unsigned i = 0; i < 100; ++i)
    text += "int value_" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
  return text;
}
void edited_text(const fs::path &root, std::optional<Key> key, unsigned layout) {
  Fixture f(root, key);
  const auto before = source_text();
  auto after = before;
  after.replace(after.find("= 37;"), 5, "= 99;");
  const auto first = f.revision(Json{{"Old/Édit.cpp", f.file(before, false, 1024)}});
  const auto next = f.revision(Json{{"New/édit.cpp", f.file(after, true, 2048)}}, Json::array({first}));
  f.install(next);
  f.pack(layout);
  const auto physical = f.physical();
  require(native_diff_renames(f.store, first, next)["counts"]["renamed"] == 0,
          "legacy exact mode guessed an edited rename");
  const auto report = native_diff_renames(f.store, first, next, NativeRenameOptions{}, {});
  require(report["counts"]["renamed"] == 1 && report["changes"].size() == 1,
          "small edited source file was not tracked");
  const auto &row = report["changes"][0];
  require(row["old_path"] == "Old/Édit.cpp" && row["path"] == "New/édit.cpp" &&
              row["content_changed"] == true && row["executable_changed"] == true &&
              row["similarity"].get<unsigned>() >= 95,
          "edited rename lost path, mode or content evidence");
  NativeFileHistoryOptions settings;
  settings.renames.emplace();
  const auto history = native_file_history(f.store, next, "New/édit.cpp", settings);
  require(history["complete"] == true && history["entries"].size() == 2 &&
              history["entries"][1]["path_at_revision"] == "Old/Édit.cpp",
          "history stopped at an edited rename");
  require(native_file_history(f.store, next, "New/édit.cpp")["entries"].size() == 1,
          "legacy history changed implicit payload requirements");
  NativeBlameOptions attribution;
  attribution.renames.emplace();
  const auto blame = native_blame(f.store, next, "New/édit.cpp", attribution);
  require(blame["status"] == "complete" && blame["lines"].size() == 100,
          "edited-rename blame did not complete");
  for (std::size_t i = 0; i < 100; ++i) {
    const auto &line = blame["lines"][i];
    require(line["revision"] == (i == 37 ? next : first) &&
                line["path"] == (i == 37 ? "New/édit.cpp" : "Old/Édit.cpp"),
            "edited-rename blame assigned an unchanged line to the rename commit");
  }
  require(blame["input_bytes"].get<std::size_t>() >=
              blame["rename_detection"]["input_bytes"].get<std::size_t>() + before.size() + after.size(),
          "blame omitted similarity reads from its aggregate input budget");
  attribution.renames->max_work = 1;
  const auto limited = native_blame(f.store, next, "New/édit.cpp", attribution);
  require(limited["status"] == "limited" && limited["reason"] == "rename_limit" && limited["lines"].empty() &&
              limited["commits"].empty(),
          "rename-limited blame exposed partial attribution");
  attribution.renames->max_work = 1000000;
  attribution.max_input_bytes = blame["input_bytes"].get<std::size_t>() - 1;
  const auto aggregate = native_blame(f.store, next, "New/édit.cpp", attribution);
  require(aggregate["status"] == "limited" && aggregate["reason"] == "input_limit" &&
              aggregate["lines"].empty(),
          "blame ignored the shared aggregate input budget");
  require(f.physical() == physical, "edited rename analysis modified a Store or workspace");
}
void binary_and_weights(const fs::path &root) {
  Fixture f(root);
  const std::string a(262144, '\0'), b(262144, '\1'), c(262144, '\2'), d(262144, '\3');
  const auto first = f.revision(Json{{"old.uasset", f.file(a + b + c, false, 65536)}});
  const auto next = f.revision(Json{{"new.uasset", f.file(a + d + c, false, 65536)}}, Json::array({first}));
  const auto physical = f.physical();
  const auto report = native_diff_renames(f.store, first, next, NativeRenameOptions{}, {});
  require(report["counts"]["renamed"] == 1 && report["changes"][0]["similarity"] == 66,
          "large binary block similarity is incorrect");
  require(f.physical() == physical, "binary similarity analysis wrote storage");
  const auto repeated = f.revision(Json{{"repeat.bin", f.file(a + a + a + a, false, 65536)}});
  const auto different = f.revision(Json{{"other.bin", f.file(a + b + c + d, false, 65536)}});
  require(native_diff_renames(f.store, repeated, different, NativeRenameOptions{}, {})["counts"]["renamed"] ==
              0,
          "repeated source blocks counted target bytes more than once");
}
void ambiguity_and_limits(const fs::path &root) {
  Fixture f(root);
  auto before = source_text(), left = before, right = before;
  left.replace(left.find("= 37;"), 5, "= 98;");
  right.replace(right.find("= 37;"), 5, "= 99;");
  const auto first = f.revision(Json{{"old.cpp", f.file(before)}});
  const auto tied =
      f.revision(Json{{"left.cpp", f.file(left)}, {"right.cpp", f.file(right)}}, Json::array({first}));
  const auto report = native_diff_renames(f.store, first, tied, NativeRenameOptions{}, {});
  require(report["counts"]["renamed"] == 0 && report["rename_detection"]["ambiguous_sources"] == 1 &&
              report["rename_detection"]["ambiguous_targets"] == 2,
          "equal similarity scores guessed ancestry");
  NativeFileHistoryOptions history;
  history.renames.emplace();
  require(native_file_history(f.store, tied, "left.cpp", history)["stop_reason"] == "ambiguous_rename",
          "ambiguous edited lineage was hidden");
  const auto next = f.revision(Json{{"new.cpp", f.file(left)}}, Json::array({first}));
  const auto physical = f.physical();
  for (const auto &limited : std::array<NativeRenameOptions, 5>{
           NativeRenameOptions{0}, NativeRenameOptions{101}, NativeRenameOptions{50, 1},
           NativeRenameOptions{50, 4096, 1}, NativeRenameOptions{50, 4096, 1000000, 1024}})
    fails([&] { native_diff_renames(f.store, first, next, limited, {}); });
  std::stop_source stop;
  stop.request_stop();
  fails([&] { native_diff_renames(f.store, first, next, NativeRenameOptions{}, {stop.get_token()}); });
  history.follow_renames = false;
  fails([&] { native_file_history(f.store, next, "new.cpp", history); });
  require(f.physical() == physical, "limited or cancelled analysis wrote repository state");
}
void exact_fraction_ranking(const fs::path &root) {
  Fixture f(root);
  const auto before = source_text();
  auto after = before;
  after.replace(after.find("= 37;"), 5, "= 99;");
  const auto first = f.revision(
      Json{{"old-a.cpp", f.file(before + "file_A\n")}, {"old-b.cpp", f.file(before + "file_B\n")}});
  const auto next =
      f.revision(Json{{"new-a.cpp", f.file(after + "file_A\n")}, {"new-b.cpp", f.file(after + "file_B\n")}});
  const auto report = native_diff_renames(f.store, first, next, NativeRenameOptions{}, {});
  require(report["counts"]["renamed"] == 2, "rounded percentages hid distinct best rename candidates");
  for (const auto &row : report["changes"])
    require(row["old_path"] == (row["path"] == "new-a.cpp" ? "old-a.cpp" : "old-b.cpp"),
            "similar source files were paired with the wrong unique marker");
}
void exact_metadata_only(const fs::path &root) {
  Fixture f(root);
  const auto file = f.file("payload need not be loaded for an exact match");
  const auto first = f.revision(Json{{"old.bin", file}});
  const auto next = f.revision(Json{{"new.bin", file}}, Json::array({first}));
  const auto manifest = f.store.metadata(Kind::manifest, file["manifest"].get<std::string>());
  const auto id = manifest["blocks"][0]["id"].get<std::string>();
  fs::remove(f.store.root() / "objects/chunk" / id.substr(0, 2) / id);
  f.store.clear_cache();
  const auto before = f.physical();
  const auto report = native_diff_renames(f.store, first, next, NativeRenameOptions{50, 1, 1, 1024}, {});
  require(report["counts"]["renamed"] == 1 && report["rename_detection"]["input_bytes"] == 0 &&
              f.physical() == before,
          "similarity mode reconstructed an already exact rename");
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
          key->fill(73);
        }
        edited_text(temporary.path / (std::to_string(encrypted) + '-' + std::to_string(layout)), key, layout);
      }
    binary_and_weights(temporary.path / "binary");
    ambiguity_and_limits(temporary.path / "limits");
    exact_fraction_ranking(temporary.path / "ranking");
    exact_metadata_only(temporary.path / "exact-metadata");
    std::cout
        << "Edited text/binary renames, conservative ties, shared history budgets and read-only limits\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
