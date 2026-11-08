#include "native_history_fixture.hpp"
#include <syrnck/history.hpp>
#include <iostream>
#include <random>
using namespace native_test;
namespace {
void scenario(const fs::path &root, std::optional<Key> key, unsigned layout) {
  Fixture f(root, key);
  const std::string old = "old/code.cpp", renamed = "new/code-é.cpp";
  const auto a = f.revision(Json{{old, f.file("root\r\ncommon\r\n")}});
  const auto b = f.revision(Json{{old, f.file("left\r\ncommon\r\n")}}, Json::array({a}));
  const auto c = f.revision(Json{{old, f.file("root\r\ncommon\r\nright")}}, Json::array({a}));
  const auto merged = f.file("left\r\ncommon\r\nright");
  const auto d = f.revision(Json{{old, merged}}, Json::array({b, c}));
  const auto e = f.revision(Json{{renamed, f.file("left\r\ncommon\r\nright", true, 3)}}, Json::array({d}));
  f.install(e);
  f.pack(layout);
  const auto before = f.physical();
  const auto report = native_blame(f.store, e, renamed);
  require(report["status"] == "complete" && report["traversal"] == "all-parents" &&
              report["lines"].size() == 3 && report["commits"].size() == 3,
          "blame did not finish a merge through a reencoded rename");
  const auto &lines = report["lines"];
  require(lines[0]["revision"] == b && lines[0]["source_line"] == 1 && lines[0]["content"] == "left\r\n" &&
              lines[1]["revision"] == a && lines[1]["source_line"] == 2 && lines[1]["content"] == "common\r\n" &&
              lines[2]["revision"] == c && lines[2]["source_line"] == 3 && lines[2]["content"] == "right",
          "blame lost a parent attribution or exact line terminator");
  for (const auto &line : lines)
    require(line["path"] == old, "blame did not retain the original Unicode-safe path");
  NativeBlameOptions options;
  options.first_parent = true;
  const auto first = native_blame(f.store, e, renamed, options);
  require(first["lines"][0]["revision"] == b && first["lines"][1]["revision"] == a &&
              first["lines"][2]["revision"] == d, "first-parent blame followed a merge's second parent");
  options = {};
  options.follow_renames = false;
  const auto no_follow = native_blame(f.store, e, renamed, options);
  for (const auto &line : no_follow["lines"])
    require(line["revision"] == e && line["path"] == renamed, "disabled rename following crossed an addition");
  for (unsigned budget = 0; budget < 5; ++budget) {
    options = {};
    if (budget == 0) options.max_revisions = 1;
    if (budget == 1) options.max_input_bytes = 1;
    if (budget == 2) options.text.max_file_bytes = 1;
    if (budget == 3) options.text.max_lines = 1;
    if (budget == 4) options.text.max_work = 1;
    const auto limited = native_blame(f.store, e, renamed, options);
    require(limited["status"] == "limited" && limited["lines"].empty() && limited["commits"].empty(),
            "blame budget exposed partial or fabricated attribution");
  }
  fails([&] { native_blame(f.store, e, "absent"); });
  fails([&] { native_blame(f.store, e, "../outside"); });
  options = {};
  options.max_revisions = 0;
  fails([&] { native_blame(f.store, e, renamed, options); });
  std::stop_source stop;
  stop.request_stop();
  fails([&] { native_blame(f.store, e, renamed, {}, {stop.get_token()}); });
  std::stop_source later;
  fails([&] { native_blame(f.store, e, renamed, {}, {later.get_token(), [&](const ProgressEvent &) {
                 later.request_stop();
               }}); });
  require(f.physical() == before, "blame changed a read-only store");
  f.store.gc(true);
  require(native_blame(f.store, e, renamed)["lines"] == lines, "blame lost retained history after GC");
}
void edge_cases(const fs::path &root) {
  Fixture f(root);
  const auto binary = f.revision(Json{{"code", f.file(std::string("a\0b", 3))}});
  auto report = native_blame(f.store, binary, "code");
  require(report["status"] == "binary" && report["lines"].empty(), "binary blame fabricated text");
  const auto empty = f.revision(Json{{"code", f.file("")}}, Json::array({binary}));
  report = native_blame(f.store, empty, "code");
  require(report["status"] == "complete" && report["lines"].empty(), "empty file gained a phantom line");
  const auto text_revision = f.revision(Json{{"code", f.file("text\n")}}, Json::array({binary}));
  require(native_blame(f.store, text_revision, "code")["lines"][0]["revision"] == text_revision,
          "text introduced over binary content was attributed to binary data");
  const auto data = f.file("duplicate\n");
  const auto duplicate = f.revision(Json{{"old1", data}, {"old2", data}});
  const auto next = f.revision(Json{{"new", data}}, Json::array({duplicate}));
  require(native_blame(f.store, next, "new")["lines"][0]["revision"] == next,
          "blame guessed between ambiguous removed sources");
  const auto single = f.revision(Json{{"old", data}});
  const auto copies = f.revision(Json{{"new1", data}, {"new2", data}}, Json::array({single}));
  require(native_blame(f.store, copies, "new1")["lines"][0]["revision"] == copies,
          "blame guessed between ambiguous added copies");
  std::string many;
  for (unsigned i = 0; i < 100; ++i) many += "line\n";
  const auto large = f.revision(Json{{"code", f.file(many)}});
  NativeBlameOptions limits;
  limits.text.max_output_bytes = 1024;
  report = native_blame(f.store, large, "code", limits);
  require(report["status"] == "limited" && report["reason"] == "output_limit" && report["lines"].empty() &&
              report["commits"].empty(), "output-limited blame exposed a partial answer");
  const std::string long_path(900, 'a');
  const auto long_name = f.revision(Json{{long_path, f.file("")}});
  fails([&] { native_blame(f.store, long_name, long_path, limits); });
  const auto manifest = f.store.metadata(Kind::manifest, data["manifest"].get<std::string>());
  const auto block = manifest["blocks"][0]["id"].get<std::string>();
  const auto path = f.store.root() / "objects/chunk" / block.substr(0, 2) / block;
  atomic_write(path, bytes("corrupt envelope"), true);
  f.store.clear_cache();
  fails([&] { native_blame(f.store, next, "new"); });
}
void generated_lineage(const fs::path &root) {
  Fixture f(root);
  struct Expected { std::string text, revision; std::size_t line; };
  std::vector<Expected> lines;
  std::mt19937 random(52917);
  std::optional<std::string> parent;
  for (unsigned step = 0; step < 80; ++step) {
    const auto choice = random() % 3;
    if (lines.empty() || choice == 0) {
      const auto at = static_cast<std::size_t>(random() % static_cast<unsigned>(lines.size() + 1));
      lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(at),
                   {"introduced " + std::to_string(step) + "\n", {}, 0});
    } else {
      const auto at = static_cast<std::size_t>(random() % static_cast<unsigned>(lines.size()));
      if (choice == 1)
        lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(at));
      else
        lines[at] = {"replaced " + std::to_string(step) + "\n", {}, 0};
    }
    std::string content;
    for (const auto &line : lines) content += line.text;
    const auto id = f.revision(Json{{"code", f.file(content)}}, parent ? Json::array({*parent}) : Json::array());
    for (std::size_t i = 0; i < lines.size(); ++i)
      if (lines[i].revision.empty()) {
        lines[i].revision = id;
        lines[i].line = i + 1;
      }
    parent = id;
    const auto report = native_blame(f.store, id, "code");
    require(report["status"] == "complete" && report["lines"].size() == lines.size(),
            "generated blame lineage was incomplete");
    for (std::size_t i = 0; i < lines.size(); ++i)
      require(report["lines"][i]["revision"] == lines[i].revision &&
                  report["lines"][i]["source_line"] == lines[i].line &&
                  report["lines"][i]["content"] == lines[i].text,
              "generated blame lineage differs from independently tracked insertions/deletions/edits");
  }
}
} // namespace
int main() {
  Temporary temporary;
  try {
    for (bool encrypted : {false, true})
      for (unsigned layout = 0; layout < 3; ++layout) {
        std::optional<Key> key;
        if (encrypted) { key.emplace(); key->fill(29); }
        scenario(temporary.path / (std::to_string(encrypted) + '-' + std::to_string(layout)), key, layout);
      }
    edge_cases(temporary.path / "edges");
    generated_lineage(temporary.path / "generated");
    std::cout << "Verified native line attribution through merge parents, renames, budgets and GC\n";
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
