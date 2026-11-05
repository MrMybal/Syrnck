#include "internal.hpp"
#include <iostream>
#include <sstream>
using namespace syrnck;
namespace {
template <class F> void fails(F fn, std::string_view expected = {}) {
  try {
    fn();
  } catch (const Error &e) {
    require(expected.empty() || std::string_view(e.what()) == expected,
            "unexpected reset-hunk error: " + std::string(e.what()));
    return;
  }
  throw Error("expected reset-hunk failure");
}
void write(const fs::path &path, std::string_view value) {
  fs::create_directories(path.parent_path());
  atomic_write(path, bytes(value), true);
}
std::string body(unsigned mask) {
  std::string result;
  for (unsigned n = 0; n < 25; ++n) {
    if ((n == 2 && (mask & 1)) || (n == 12 && (mask & 2)) || (n == 22 && (mask & 4)))
      result += "changed ";
    result += "line " + std::to_string(n) + '\n';
  }
  return result;
}
std::string contents(const Store &store, std::string_view revision, const std::string &path = "code.cpp") {
  const auto value = store.metadata(Kind::revision, revision);
  std::ostringstream output;
  store.reconstruct(value["files"][path]["manifest"].get<std::string>(), output);
  return output.str();
}
std::map<std::string, std::string> physical(const Store &store) {
  std::map<std::string, std::string> result;
  for (const auto &entry : fs::recursive_directory_iterator(store.root()))
    if (entry.is_regular_file() && entry.path().filename() != "store.lock")
      result.emplace(utf8(entry.path().lexically_relative(store.root())), hash(read_bytes(entry.path())));
  return result;
}
std::string id(const Json &plan) {
  return plan["plan_id"].get<std::string>();
}
bool blocked(const Json &plan, std::string_view reason) {
  for (const auto &entry : plan["blockers"])
    if (entry["reason"].get<std::string>() == reason)
      return true;
  return false;
}
void scenario(const fs::path &project, std::optional<Key> key, unsigned layout) {
  Store store(project / ".syrnck", key, true);
  write(project / "code.cpp", body(0));
  write(project / "other.txt", "original other");
  const auto head = store.commit_directory(project, "main", "baseline");
  require(blocked(store.plan_reset_hunks("main", "code.cpp"), "staging_required"), "missing index accepted");
  write(project / "code.cpp", body(7));
  write(project / "other.txt", "frozen other");
  const auto staged = store.stage_directory(project, "main", {"."});
  auto snapshot = store.metadata(Kind::revision, staged);
  snapshot["files"]["code.cpp"]["executable"] = true;
  const auto index = store.put(Kind::revision, bytes(snapshot.dump())).first;
  store.set_ref("staging/main", index);
  if (layout)
    store.pack(layout == 1 ? PackOptions{} : PackOptions{512 * 1024 * 1024, 2});
  // No workspace is needed, even for a path now ignored or replaced by a directory.
  fs::remove(project / "code.cpp");
  fs::create_directory(project / "code.cpp");
  write(project / ".syrnckignore", "code.cpp\n");
  const auto untouched = physical(store);
  const auto plan = store.plan_reset_hunks("main", "code.cpp");
  const auto diff = store.diff_staged_file("main", "code.cpp");
  require(plan["ready"] == true && plan["operation"] == "reset-hunks" && plan["diff"]["hunks"].size() == 3 &&
              id(plan) == id(store.plan_reset_hunks("main", "code.cpp")) && plan["source_revision"] == head &&
              plan["target_revision"] == index.id && physical(store) == untouched,
          "reset preview changed objects, direction or identity");
  for (std::size_t n = 0; n < 3; ++n) {
    auto numbered = plan["diff"]["hunks"][n];
    numbered.erase("number");
    require(numbered == diff["diff"]["hunks"][n], "reset numbering differs from staged diff");
  }
  for (unsigned mask = 1; mask <= 7; ++mask) {
    store.set_ref("staging/main", index);
    const auto preview = store.plan_reset_hunks("main", "code.cpp");
    std::vector<std::size_t> selected;
    for (std::size_t n = 1; n <= 3; ++n)
      if (mask & (1U << (n - 1)))
        selected.push_back(n);
    const auto applied = store.reset_hunks("main", "code.cpp", id(preview), selected);
    const auto next = applied["index"].get<std::string>();
    const auto frozen = store.metadata(Kind::revision, next);
    require(contents(store, next) == body(7 ^ mask) && frozen["files"]["code.cpp"]["executable"] == true &&
                frozen["files"]["other.txt"] == snapshot["files"]["other.txt"] &&
                frozen["parents"] == Json::array({head}) && applied["selected"] == Json(selected) &&
                applied["previous_index"] == index.id && store.refs().at("heads/main").id == head &&
                store.refs().at("archive/stage-index/" + index.id) == index &&
                fs::is_directory(project / "code.cpp"),
            "reset hunks lost frozen entries/mode or changed workspace/head");
  }
  store.set_ref("staging/main", index);
  const auto current = store.plan_reset_hunks("main", "code.cpp");
  const auto before = physical(store);
  for (const auto &numbers : {std::vector<std::size_t>{}, {0}, {1, 1}, {4}})
    fails([&] { store.reset_hunks("main", "code.cpp", id(current), numbers); });
  TextDiffOptions different;
  different.context = 0;
  fails([&] { store.reset_hunks("main", "code.cpp", id(current), {1}, different); },
        "native hunk plan changed; refresh the plan");
  require(physical(store) == before, "invalid reset wrote objects");
  for (const auto phase : {ProgressPhase::ingesting, ProgressPhase::publishing}) {
    const auto refs = store.refs();
    std::stop_source stop;
    fails(
        [&] {
          store.reset_hunks("main", "code.cpp", id(current), {2}, {},
                            {stop.get_token(), [&](const ProgressEvent &event) {
                               if (event.phase == phase &&
                                   (phase != ProgressPhase::publishing || event.path == "refs"))
                                 stop.request_stop();
                             }});
        },
        "operation cancelled");
    require(store.refs() == refs, "cancelled reset published references");
  }
  const auto refs = store.refs();
  fails(
      [&] {
        store.reset_hunks("main", "code.cpp", id(current), {2}, {},
                          {{}, [](const ProgressEvent &event) {
                             if (event.phase == ProgressPhase::publishing && event.path == "refs")
                               throw Error("reset observer sentinel");
                           }});
      },
      "reset observer sentinel");
  require(store.refs() == refs, "failed observer published reset");
  const auto applied = store.reset_hunks("main", "code.cpp", id(current), {2});
  const auto saved = physical(store);
  fails([&] { store.reset_hunks("main", "code.cpp", id(current), {1}); },
        "native hunk plan changed; refresh the plan");
  require(physical(store) == saved, "stale plan wrote objects");
  store.gc(true);
  require(contents(store, index.id) == body(7), "GC removed archived index");
  const auto committed = store.commit_staged("main", "keep first and last");
  store.gc(true);
  require(contents(store, committed) == body(5) &&
              contents(store, committed, "other.txt") == "frozen other" &&
              contents(store, index.id) == body(7) && fs::is_directory(project / "code.cpp"),
          "reset result/previous index was lost after commit or GC");
}
void edge_cases(const fs::path &project) {
  Store store(project / ".syrnck", {}, true);
  const auto code = project / "code.cpp";
  for (const auto &[old, next] : std::vector<std::pair<std::string, std::string>>{
           {"", "new\r\n"}, {"old\r\n", ""}, {"old", "old\n"}, {"a\r\nb\nlast", "a\r\nc\nend"}}) {
    store.reset_stage("main");
    write(code, old);
    store.commit_directory(project, "main", "newline base");
    write(code, next);
    store.stage_directory(project, "main", {"."});
    fs::remove(code);
    const auto plan = store.plan_reset_hunks("main", "code.cpp");
    const auto result = store.reset_hunks("main", "code.cpp", id(plan), {1});
    require(contents(store, result["index"].get<std::string>()) == old && !fs::exists(code),
            "reset normalized newlines");
  }
  store.reset_stage("main");
  write(code, body(0));
  const auto head = store.commit_directory(project, "main", "base");
  write(code, body(7));
  const auto index = store.stage_directory(project, "main", {"."});
  auto advanced = store.metadata(Kind::revision, head);
  advanced["parents"] = Json::array({head});
  advanced["message"] = "advance";
  const auto next = store.put(Kind::revision, bytes(advanced.dump())).first;
  store.set_ref("heads/main", next);
  auto plan = store.plan_reset_hunks("main", "code.cpp");
  require(blocked(plan, "stale_staging"), "stale index accepted");
  auto before = physical(store);
  fails([&] { store.reset_hunks("main", "code.cpp", id(plan), {1}); });
  require(before == physical(store), "stale reset wrote objects");
  store.set_ref("heads/main", {Kind::revision, head});
  store.set_ref("archive/stage-index/" + index, next);
  plan = store.plan_reset_hunks("main", "code.cpp");
  require(blocked(plan, "archive_conflict"), "archive collision accepted");
  before = physical(store);
  fails([&] { store.reset_hunks("main", "code.cpp", id(plan), {1}); });
  require(before == physical(store), "archive collision wrote objects");
  store.remove_ref("archive/stage-index/" + index);
  TextDiffOptions tiny;
  tiny.max_lines = 1;
  plan = store.plan_reset_hunks("main", "code.cpp", tiny);
  require(blocked(plan, "text_diff_required") && plan["diff"]["reason"] == "line_limit",
          "limited reset accepted");
  before = physical(store);
  fails([&] { store.reset_hunks("main", "code.cpp", id(plan), {1}, tiny); });
  require(before == physical(store), "limited reset wrote objects");
  // Corruption of the selected baseline must prevent even a preview.
  const auto snapshot = store.metadata(Kind::revision, head);
  const auto manifest =
      store.metadata(Kind::manifest, snapshot["files"]["code.cpp"]["manifest"].get<std::string>());
  const auto chunk = manifest["blocks"][0]["id"].get<std::string>();
  fs::remove(store.root() / "objects/chunk" / chunk.substr(0, 2) / chunk);
  fails([&] { store.plan_reset_hunks("main", "code.cpp"); });
}
std::string edits(unsigned mask) {
  std::string result = mask & 1 ? "inserted first\ninserted second\n" : "";
  for (unsigned n = 0; n < 40; ++n)
    if (!(mask & 2) || (n != 12 && n != 13))
      result += "original " + std::to_string(n) + '\n';
  if (mask & 4)
    result += "appended without newline";
  return result;
}
void different_line_counts(const fs::path &project) {
  Store store(project / ".syrnck", {}, true);
  write(project / "code.cpp", edits(0));
  store.commit_directory(project, "main", "insertion/deletion base");
  write(project / "code.cpp", edits(7));
  const auto index = store.stage_directory(project, "main", {"."});
  TextDiffOptions limits;
  limits.context = 0;
  for (unsigned mask = 1; mask <= 7; ++mask) {
    store.set_ref("staging/main", {Kind::revision, index});
    const auto plan = store.plan_reset_hunks("main", "code.cpp", limits);
    require(plan["ready"] == true && plan["diff"]["hunks"].size() == 3, "insertion/deletion fixture differs");
    std::vector<std::size_t> numbers;
    for (std::size_t n = 1; n <= 3; ++n)
      if (mask & (1U << (n - 1)))
        numbers.push_back(n);
    const auto result = store.reset_hunks("main", "code.cpp", id(plan), numbers, limits);
    require(contents(store, result["index"].get<std::string>()) == edits(7 ^ mask),
            "reset mishandled zero-count ranges, shifted lines or final newline");
  }
  store.reset_stage("main");
  const auto common = body(0);
  write(project / "code.cpp", std::string(1800, 'a') + '\n' + common);
  store.commit_directory(project, "main", "bounded reset base");
  write(project / "code.cpp", common + std::string(1800, 'b') + '\n');
  store.stage_directory(project, "main", {"."});
  limits.max_file_bytes = 2500;
  const auto plan = store.plan_reset_hunks("main", "code.cpp", limits);
  require(plan["ready"] == true && plan["diff"]["hunks"].size() == 2, "bounded reset fixture differs");
  const auto before = physical(store);
  fails([&] { store.reset_hunks("main", "code.cpp", id(plan), {1}, limits); },
        "native hunk result exceeds file budget");
  require(before == physical(store), "oversized reset wrote objects");
}
void incomplete_closure(const fs::path &project) {
  Store store(project / ".syrnck", {}, true);
  write(project / "code.cpp", body(0));
  write(project / "other.txt", "unrelated retained data");
  const auto head = store.commit_directory(project, "main", "full baseline");
  write(project / "code.cpp", body(7));
  store.stage_directory(project, "main", {"."});
  const auto snapshot = store.metadata(Kind::revision, head);
  const auto manifest =
      store.metadata(Kind::manifest, snapshot["files"]["other.txt"]["manifest"].get<std::string>());
  const auto chunk = manifest["blocks"][0]["id"].get<std::string>();
  fs::remove(store.root() / "objects/chunk" / chunk.substr(0, 2) / chunk);
  const auto plan = store.plan_reset_hunks("main", "code.cpp");
  require(plan["ready"] == true, "reset preview read unrelated payload");
  const auto refs = store.refs();
  fails([&] { store.reset_hunks("main", "code.cpp", id(plan), {1}); });
  require(store.refs() == refs && text(read_bytes(project / "code.cpp")) == body(7),
          "reset published incomplete retained closure");
}
} // namespace
int main() {
  const auto root = fs::temp_directory_path() / ("syrnck-reset-hunks-" + hex(random_bytes(16)));
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code ec;
      fs::remove_all(path, ec);
    }
  } cleanup{root};
  try {
    for (bool encrypted : {false, true})
      for (unsigned layout = 0; layout < 3; ++layout) {
        std::optional<Key> key;
        if (encrypted) {
          key.emplace();
          key->fill(49);
        }
        scenario(root / (std::to_string(encrypted) + '-' + std::to_string(layout)), key, layout);
      }
    edge_cases(root / "edges");
    different_line_counts(root / "counts");
    incomplete_closure(root / "incomplete");
    std::cout
        << "Verified reset subsets, exact bytes, workspace independence, archives, cancellation and GC\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
