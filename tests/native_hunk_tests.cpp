#include "internal.hpp"
#include <fstream>
#include <iostream>
#include <source_location>
#include <sstream>
using namespace syrnck;
namespace {
template <class F>
void fails(F fn, std::string_view expected = {},
           std::source_location location = std::source_location::current()) {
  try {
    fn();
  } catch (const Error &e) {
    require(expected.empty() || std::string_view(e.what()) == expected,
            "unexpected hunk failure at " + std::to_string(location.line()) + ": " + e.what());
    return;
  }
  throw Error("expected hunk failure at " + std::to_string(location.line()));
}
void write(const fs::path &path, std::string_view value) {
  fs::create_directories(path.parent_path());
  atomic_write(path, bytes(value), true);
}
std::string body(unsigned mask) {
  std::string result;
  for (unsigned line = 0; line < 25; ++line) {
    if ((line == 2 && (mask & 1)) || (line == 12 && (mask & 2)) || (line == 22 && (mask & 4)))
      result += "changed ";
    result += "line " + std::to_string(line) + '\n';
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
std::string identity(const Json &plan) {
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
  const auto code = project / "code.cpp";
  write(code, body(0));
  write(project / "other.txt", "old other");
  const auto initial = store.commit_directory(project, "main", "baseline");
  auto baseline = store.metadata(Kind::revision, initial);
  baseline["files"]["code.cpp"]["executable"] = true;
  const auto head = store.put(Kind::revision, bytes(baseline.dump())).first;
  store.set_ref("heads/main", head);
  if (layout)
    store.pack(layout == 1 ? PackOptions{} : PackOptions{512 * 1024 * 1024, 2});
  write(code, body(7));
  const auto before = physical(store);
  const auto plan = store.plan_stage_hunks(project, "main", "code.cpp");
  require(plan["ready"] == true && plan["diff"]["hunks"].size() == 3 &&
              plan["diff"]["hunks"][0]["number"] == 1 && plan["index"].is_null() &&
              physical(store) == before &&
              identity(plan) == identity(store.plan_stage_hunks(project, "main", "code.cpp")),
          "hunk preview is not read-only/deterministic");
  for (unsigned mask = 1; mask <= 7; ++mask) {
    store.reset_stage("main");
    const auto preview = store.plan_stage_hunks(project, "main", "code.cpp");
    std::vector<std::size_t> selected;
    for (std::size_t n = 1; n <= 3; ++n)
      if (mask & (1U << (n - 1)))
        selected.push_back(n);
    const auto applied = store.stage_hunks(project, "main", "code.cpp", identity(preview), selected);
    const auto index = applied["index"].get<std::string>();
    const auto frozen = store.metadata(Kind::revision, index);
    require(applied["state"] == "applied" && contents(store, index) == body(mask) &&
                frozen["files"]["other.txt"] == baseline["files"]["other.txt"] &&
                frozen["files"]["code.cpp"]["executable"] == true &&
                frozen["parents"] == Json::array({head.id}) && frozen["timestamp"] == 0 &&
                frozen["message"] == "Syrnck staging v1: main" && store.refs().at("heads/main") == head &&
                text(read_bytes(code)) == body(7),
            "selected hunks changed unselected data, mode, branch or workspace");
  }
  store.reset_stage("main");
  const auto preview = store.plan_stage_hunks(project, "main", "code.cpp");
  const auto no_writes = physical(store);
  fails([&] { store.stage_hunks(project, "main", "code.cpp", identity(preview), {}); });
  for (const auto &selection : {std::vector<std::size_t>{0}, {1, 1}, {4}})
    fails([&] { store.stage_hunks(project, "main", "code.cpp", identity(preview), selection); });
  TextDiffOptions different;
  different.context = 0;
  fails([&] { store.stage_hunks(project, "main", "code.cpp", identity(preview), {1}, different); },
        "native hunk plan changed; refresh the plan");
  write(code, body(3));
  fails([&] { store.stage_hunks(project, "main", "code.cpp", identity(preview), {1}); },
        "native hunk plan changed; refresh the plan");
  write(code, body(7));
  require(physical(store) == no_writes, "invalid hunk selection wrote Store data");
  for (const auto phase : {ProgressPhase::ingesting, ProgressPhase::publishing}) {
    const auto refs = store.refs();
    std::stop_source stopped;
    fails(
        [&] {
          store.stage_hunks(project, "main", "code.cpp", identity(preview), {1}, {},
                            {stopped.get_token(), [&](const ProgressEvent &event) {
                               if (event.phase == phase &&
                                   (phase != ProgressPhase::publishing || event.path == "refs"))
                                 stopped.request_stop();
                             }});
        },
        "operation cancelled");
    require(store.refs() == refs, "cancelled hunk staging changed references");
  }
  const auto refs = store.refs();
  fails(
      [&] {
        store.stage_hunks(project, "main", "code.cpp", identity(preview), {1}, {},
                          {{}, [](const ProgressEvent &event) {
                             if (event.phase == ProgressPhase::publishing && event.path == "refs")
                               throw Error("hunk observer sentinel");
                           }});
      },
      "hunk observer sentinel");
  require(store.refs() == refs, "observer failure published hunks");
  fails(
      [&] {
        store.stage_hunks(project, "main", "code.cpp", identity(preview), {1}, {},
                          {{}, [&](const ProgressEvent &event) {
                             if (event.phase == ProgressPhase::publishing && event.path == "refs")
                               write(code, body(3));
                           }});
      },
      "file changed during native text diff; retry operation");
  require(store.refs() == refs, "late working modification published hunks");
  write(code, body(7));
  write(project / "other.txt", "prepared other");
  const auto previous = store.stage_directory(project, "main", {"other.txt"});
  const auto saved = physical(store);
  fails([&] { store.stage_hunks(project, "main", "code.cpp", identity(preview), {1}); },
        "native hunk plan changed; refresh the plan");
  require(physical(store) == saved, "stale index preview wrote objects");
  const auto current = store.plan_stage_hunks(project, "main", "code.cpp");
  const auto applied = store.stage_hunks(project, "main", "code.cpp", identity(current), {2});
  const auto next = applied["index"].get<std::string>();
  const auto archive = "archive/stage-index/" + previous;
  require(store.refs().at(archive).id == previous && contents(store, next) == body(2) &&
              contents(store, next, "other.txt") == "prepared other",
          "hunks lost existing preparations");
  store.gc(true);
  require(contents(store, previous) == body(0) && contents(store, next) == body(2) &&
              store.native_archives("stage-index", {}, 100, true)["archives"].size() == 1,
          "previous index or selected data was lost after GC");
  const auto committed = store.commit_staged("main", "selected middle change");
  store.gc(true);
  require(contents(store, committed) == body(2) && contents(store, head.id) == body(0) &&
              contents(store, previous, "other.txt") == "prepared other" && text(read_bytes(code)) == body(7),
          "selected commit lost retained versions or changed working bytes");
  write(code, body(2));
  require(blocked(store.plan_stage_hunks(project, "main", "code.cpp"), "no_text_changes"),
          "equal text is stageable");
  write(code, std::string("binary\0text", 11));
  require(blocked(store.plan_stage_hunks(project, "main", "code.cpp"), "text_diff_required"),
          "binary text is stageable");
  fs::remove(code);
  require(blocked(store.plan_stage_hunks(project, "main", "code.cpp"), "existing_file_required"),
          "deletion is stageable as text");
  write(code, body(7));
  write(project / "new.cpp", "new file");
  require(blocked(store.plan_stage_hunks(project, "main", "new.cpp"), "existing_file_required"),
          "addition is stageable as text");
  TextDiffOptions tiny;
  tiny.max_file_bytes = 1;
  const auto limited = store.plan_stage_hunks(project, "main", "code.cpp", tiny);
  const auto unchanged = physical(store);
  fails([&] { store.stage_hunks(project, "main", "code.cpp", identity(limited), {1}, tiny); },
        "native hunk staging is blocked; inspect native add-hunks-plan");
  require(physical(store) == unchanged, "blocked hunk plan wrote objects");
}
void newline_cases(const fs::path &project) {
  Store store(project / ".syrnck", {}, true);
  for (const auto &[old, next] : std::vector<std::pair<std::string, std::string>>{
           {"", "new\r\n"}, {"old\r\n", ""}, {"old", "old\n"}, {"a\r\nb\nlast", "a\r\nc\nend"}}) {
    store.reset_stage("main");
    write(project / "code.cpp", old);
    store.commit_directory(project, "main", "newline base");
    write(project / "code.cpp", next);
    const auto plan = store.plan_stage_hunks(project, "main", "code.cpp");
    require(plan["ready"] == true && plan["diff"]["hunks"].size() == 1, "newline plan is blocked");
    const auto report = store.stage_hunks(project, "main", "code.cpp", identity(plan), {1});
    require(contents(store, report["index"].get<std::string>()) == next,
            "newline bytes changed during hunk selection");
  }
  store.reset_stage("main");
  const std::string common = body(0);
  const auto large_source = std::string(1800, 'a') + '\n' + common;
  const auto large_target = common + std::string(1800, 'b') + '\n';
  write(project / "code.cpp", large_source);
  store.commit_directory(project, "main", "bounded partial result");
  write(project / "code.cpp", large_target);
  TextDiffOptions budget;
  budget.max_file_bytes = 2500;
  const auto bounded = store.plan_stage_hunks(project, "main", "code.cpp", budget);
  require(bounded["ready"] == true && bounded["diff"]["hunks"].size() == 2,
          "bounded partial selection fixture differs");
  const auto untouched = physical(store);
  fails([&] { store.stage_hunks(project, "main", "code.cpp", identity(bounded), {2}, budget); },
        "native hunk result exceeds file budget");
  require(physical(store) == untouched, "oversized selected result wrote objects");
  write(project / "code.cpp", body(0));
  store.stage_directory(project, "unborn", {"code.cpp"});
  write(project / "code.cpp", body(7));
  const auto plan = store.plan_stage_hunks(project, "unborn", "code.cpp");
  const auto report = store.stage_hunks(project, "unborn", "code.cpp", identity(plan), {1});
  require(plan["head"].is_null() &&
              store.metadata(Kind::revision, report["index"].get<std::string>())["parents"].empty(),
          "initial index acquired a branch parent");
}
void blocked_states(const fs::path &project) {
  Store store(project / ".syrnck", {}, true);
  write(project / "code.cpp", body(0));
  const auto first = store.commit_directory(project, "main", "base");
  auto value = store.metadata(Kind::revision, first);
  value["parents"] = Json::array({first});
  value["message"] = "advanced head";
  const auto next = store.put(Kind::revision, bytes(value.dump())).first;
  const auto previous = store.stage_directory(project, "main", {"code.cpp"});
  write(project / "code.cpp", body(7));
  store.set_ref("heads/main", next);
  auto plan = store.plan_stage_hunks(project, "main", "code.cpp");
  require(blocked(plan, "stale_staging"), "stale staging did not block hunk selection");
  auto before = physical(store);
  fails([&] { store.stage_hunks(project, "main", "code.cpp", identity(plan), {1}); });
  require(physical(store) == before, "blocked stale hunk staging wrote objects");
  store.set_ref("heads/main", {Kind::revision, first});
  const auto archive = "archive/stage-index/" + previous;
  store.set_ref(archive, next);
  plan = store.plan_stage_hunks(project, "main", "code.cpp");
  require(blocked(plan, "archive_conflict"), "hunk archive collision was adopted");
  before = physical(store);
  fails([&] { store.stage_hunks(project, "main", "code.cpp", identity(plan), {1}); });
  require(physical(store) == before, "conflicting archive was overwritten");
  store.remove_ref(archive);
  store.reset_stage("main");
  write(project / "code.cpp", body(0));
  std::istringstream changed(body(1));
  const auto manifest = store.ingest(changed).first;
  value["files"]["code.cpp"]["manifest"] = manifest;
  const auto target = store.put(Kind::revision, bytes(value.dump())).first;
  std::stop_source stop;
  fails([&] {
    store.checkout_directory(project, "main", target.id,
                             {stop.get_token(), [&](const ProgressEvent &event) {
                                if (event.phase == ProgressPhase::exporting && event.bytes)
                                  stop.request_stop();
                              }});
  });
  plan = store.plan_stage_hunks(project, "main", "code.cpp");
  require(blocked(plan, "pending_checkout"), "pending checkout did not block hunk selection");
  const auto reset = store.plan_reset_hunks("main", "code.cpp");
  require(blocked(reset, "pending_checkout"), "pending checkout did not block hunk reset");
  before = physical(store);
  fails([&] { store.stage_hunks(project, "main", "code.cpp", identity(plan), {1}); });
  fails([&] { store.reset_hunks("main", "code.cpp", identity(reset), {1}); });
  require(physical(store) == before, "hunk staging modified a pending checkout");
  store.recover_checkout(true);
}
void missing_unselected_payload(const fs::path &project) {
  Store store(project / ".syrnck", {}, true);
  write(project / "code.cpp", body(0));
  write(project / "other.txt", "independent retained bytes");
  const auto first = store.commit_directory(project, "main", "complete snapshot");
  const auto snapshot = store.metadata(Kind::revision, first);
  const auto other =
      store.metadata(Kind::manifest, snapshot["files"]["other.txt"]["manifest"].get<std::string>());
  const auto chunk = other["blocks"][0]["id"].get<std::string>();
  fs::remove(store.root() / "objects/chunk" / chunk.substr(0, 2) / chunk);
  write(project / "code.cpp", body(7));
  const auto plan = store.plan_stage_hunks(project, "main", "code.cpp");
  require(plan["ready"] == true, "targeted preview read unrelated payloads");
  const auto refs = store.refs();
  fails([&] { store.stage_hunks(project, "main", "code.cpp", identity(plan), {1}); });
  require(store.refs() == refs && text(read_bytes(project / "code.cpp")) == body(7),
          "hunk staging published an incomplete full index");
}
} // namespace
int main() {
  const auto root = fs::temp_directory_path() / ("syrnck-hunks-" + hex(random_bytes(16)));
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
          key->fill(37);
        }
        scenario(root / (std::to_string(encrypted) + '-' + std::to_string(layout)), key, layout);
      }
    newline_cases(root / "newlines");
    blocked_states(root / "blocked");
    missing_unselected_payload(root / "missing");
    std::cout << "Verified all hunk subsets, frozen entries, exact newlines, atomic archives, cancellation "
                 "and GC\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
