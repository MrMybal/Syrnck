#include "internal.hpp"
#include <iostream>
#include <source_location>
#include <sstream>
using namespace syrnck;
namespace {
template <class F>
void fails(F fn, std::string_view expected = {},
           std::source_location where = std::source_location::current()) {
  try {
    fn();
  } catch (const Error &e) {
    require(expected.empty() || std::string_view(e.what()) == expected,
            "unexpected line-selection failure at " + std::to_string(where.line()) + ": " + e.what());
    return;
  }
  throw Error("expected line-selection failure at " + std::to_string(where.line()));
}
void write(const fs::path &path, std::string_view value) {
  fs::create_directories(path.parent_path());
  atomic_write(path, bytes(value), true);
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
std::vector<std::size_t> numbers(unsigned mask) {
  std::vector<std::size_t> result;
  for (std::size_t n = 1; n <= 4; ++n)
    if (mask & (1U << (n - 1)))
      result.push_back(n);
  return result;
}
// An independent fixture oracle: two old rows followed by two new rows.
std::string expected(unsigned chosen, std::string_view eol = "\n") {
  std::string result = "before" + std::string(eol);
  if (!(chosen & 1))
    result += "old A" + std::string(eol);
  if (!(chosen & 2))
    result += "old B" + std::string(eol);
  if (chosen & 4)
    result += "new A" + std::string(eol);
  if (chosen & 8)
    result += "new B" + std::string(eol);
  return result + "after" + std::string(eol);
}
void scenario(const fs::path &project, std::optional<Key> key, unsigned layout) {
  Store store(project / ".syrnck", key, true);
  write(project / "code.cpp", expected(0));
  write(project / "other.txt", "original");
  const auto first = store.commit_directory(project, "main", "baseline");
  auto value = store.metadata(Kind::revision, first);
  value["files"]["code.cpp"]["executable"] = true;
  const auto head = store.put(Kind::revision, bytes(value.dump())).first;
  store.set_ref("heads/main", head);
  write(project / "other.txt", "prepared neighbor");
  const auto partial = store.stage_directory(project, "main", {"other.txt"});
  write(project / "code.cpp", expected(15));
  const auto full = store.stage_directory(project, "main", {"code.cpp"});
  value = store.metadata(Kind::revision, full);
  value["files"]["code.cpp"]["executable"] = true;
  const auto index = store.put(Kind::revision, bytes(value.dump())).first;
  store.set_ref("staging/main", {Kind::revision, partial});
  if (layout)
    store.pack(layout == 1 ? PackOptions{} : PackOptions{512 * 1024 * 1024, 2});
  const auto before = physical(store);
  const auto plan = store.plan_stage_lines(project, "main", "code.cpp");
  require(plan["ready"] == true && plan["change_count"] == 4 && plan["selection_unit"] == "line" &&
              plan["diff"]["hunks"].size() == 1 && plan["operation"] == "stage-lines" &&
              id(plan) == id(store.plan_stage_lines(project, "main", "code.cpp")) &&
              physical(store) == before,
          "line preview is not read-only/deterministic");
  const auto &rows = plan["diff"]["hunks"][0]["lines"];
  require(!rows[0].contains("change_number") && rows[1]["kind"] == "remove" &&
              rows[1]["change_number"] == 1 && rows[2]["change_number"] == 2 && rows[3]["kind"] == "add" &&
              rows[3]["change_number"] == 3 && rows[4]["change_number"] == 4 &&
              !rows[5].contains("change_number"),
          "line change numbering or fixture edit order differs");
  for (unsigned mask = 1; mask <= 15; ++mask) {
    store.set_ref("staging/main", {Kind::revision, partial});
    const auto current = store.plan_stage_lines(project, "main", "code.cpp");
    auto chosen = numbers(mask);
    std::reverse(chosen.begin(), chosen.end());
    const auto result = store.stage_lines(project, "main", "code.cpp", id(current), chosen);
    const auto snapshot = store.metadata(Kind::revision, result["index"].get<std::string>());
    require(contents(store, result["index"].get<std::string>()) == expected(mask) &&
                snapshot["files"]["code.cpp"]["executable"] == true &&
                contents(store, result["index"].get<std::string>(), "other.txt") == "prepared neighbor" &&
                result["selected"] == Json(chosen) && store.refs().at("heads/main") == head &&
                text(read_bytes(project / "code.cpp")) == expected(15),
            "partial line staging changed unselected data");
  }
  fs::remove(project / "code.cpp");
  for (unsigned mask = 1; mask <= 15; ++mask) {
    store.set_ref("staging/main", index);
    const auto current = store.plan_reset_lines("main", "code.cpp");
    const auto result = store.reset_lines("main", "code.cpp", id(current), numbers(mask));
    const auto snapshot = store.metadata(Kind::revision, result["index"].get<std::string>());
    require(current["operation"] == "reset-lines" &&
                contents(store, result["index"].get<std::string>()) == expected(15 ^ mask) &&
                snapshot["files"]["code.cpp"]["executable"] == true &&
                contents(store, result["index"].get<std::string>(), "other.txt") == "prepared neighbor" &&
                store.refs().at("archive/stage-index/" + index.id) == index &&
                !fs::exists(project / "code.cpp"),
            "partial line reset changed unselected data or read workspace");
  }
  store.set_ref("staging/main", index);
  const auto reset = store.plan_reset_lines("main", "code.cpp");
  store.reset_lines("main", "code.cpp", id(reset), {1, 3});
  store.gc(true);
  const auto committed = store.commit_staged("main", "second replacement only");
  store.gc(true);
  require(contents(store, committed) == expected(10) && contents(store, index.id) == expected(15) &&
              contents(store, partial) == expected(0),
          "line selection or archived preparations were lost after commit/GC");
}
void safety(const fs::path &project) {
  Store store(project / ".syrnck", {}, true);
  write(project / "code.cpp", expected(0));
  write(project / "other.txt", "retained bytes");
  const auto head = store.commit_directory(project, "main", "baseline");
  write(project / "code.cpp", expected(15));
  const auto plan = store.plan_stage_lines(project, "main", "code.cpp");
  auto before = physical(store);
  for (const auto &choice : {std::vector<std::size_t>{}, {0}, {1, 1}, {5}})
    fails([&] { store.stage_lines(project, "main", "code.cpp", id(plan), choice); });
  const auto hunk = store.plan_stage_hunks(project, "main", "code.cpp");
  fails([&] { store.stage_lines(project, "main", "code.cpp", id(hunk), {1}); },
        "native text selection plan changed; refresh the plan");
  TextDiffOptions context;
  context.context = 0;
  fails([&] { store.stage_lines(project, "main", "code.cpp", id(plan), {1}, context); });
  write(project / "code.cpp", expected(4));
  fails([&] { store.stage_lines(project, "main", "code.cpp", id(plan), {1}); });
  write(project / "code.cpp", expected(15));
  require(before == physical(store), "invalid line selection wrote objects");
  const auto index = store.stage_directory(project, "main", {"code.cpp"});
  const auto reset = store.plan_reset_lines("main", "code.cpp");
  for (bool removing : {false, true}) {
    // Stage from the baseline for cancellation/late working checks.
    if (!removing)
      store.remove_ref("staging/main");
    const auto preview = removing ? reset : store.plan_stage_lines(project, "main", "code.cpp");
    for (auto phase : {ProgressPhase::ingesting, ProgressPhase::publishing}) {
      std::stop_source stop;
      const auto refs = store.refs();
      auto observer = [&](const ProgressEvent &event) {
        if (event.phase == phase && (phase != ProgressPhase::publishing || event.path == "refs"))
          stop.request_stop();
      };
      fails(
          [&] {
            if (removing)
              store.reset_lines("main", "code.cpp", id(preview), {1, 3}, {}, {stop.get_token(), observer});
            else
              store.stage_lines(project, "main", "code.cpp", id(preview), {1, 3}, {},
                                {stop.get_token(), observer});
          },
          "operation cancelled");
      require(store.refs() == refs, "cancelled line selection published references");
    }
    const auto refs = store.refs();
    auto observer = [](const ProgressEvent &event) {
      if (event.phase == ProgressPhase::publishing && event.path == "refs")
        throw Error("line observer sentinel");
    };
    fails(
        [&] {
          if (removing)
            store.reset_lines("main", "code.cpp", id(preview), {1, 3}, {}, {{}, observer});
          else
            store.stage_lines(project, "main", "code.cpp", id(preview), {1, 3}, {}, {{}, observer});
        },
        "line observer sentinel");
    require(store.refs() == refs, "line observer failure published references");
    store.set_ref("staging/main", {Kind::revision, index});
  }
  store.remove_ref("staging/main");
  const auto fresh = store.plan_stage_lines(project, "main", "code.cpp");
  const auto refs = store.refs();
  fails(
      [&] {
        store.stage_lines(project, "main", "code.cpp", id(fresh), {1, 3}, {},
                          {{}, [&](const ProgressEvent &event) {
                             if (event.phase == ProgressPhase::publishing && event.path == "refs")
                               write(project / "code.cpp", "late working edit");
                           }});
      },
      "file changed during native text diff; retry operation");
  require(store.refs() == refs, "late working edit published line selection");
  write(project / "code.cpp", expected(15));
  store.set_ref("staging/main", {Kind::revision, index});
  auto advanced = store.metadata(Kind::revision, head);
  advanced["parents"] = Json::array({head});
  advanced["message"] = "advance";
  store.set_ref("heads/main", store.put(Kind::revision, bytes(advanced.dump())).first);
  require(blocked(store.plan_reset_lines("main", "code.cpp"), "stale_staging"), "stale line reset accepted");
  store.set_ref("heads/main", {Kind::revision, head});
  store.set_ref("archive/stage-index/" + index, {Kind::revision, head});
  auto conflict = store.plan_reset_lines("main", "code.cpp");
  before = physical(store);
  fails([&] { store.reset_lines("main", "code.cpp", id(conflict), {1}); });
  require(blocked(conflict, "archive_conflict") && before == physical(store),
          "line reset overwrote archive conflict");
  store.remove_ref("archive/stage-index/" + index);
  // A targeted plan may succeed with a corrupt unrelated file; publication must verify the full closure.
  const auto snapshot = store.metadata(Kind::revision, head);
  const auto manifest =
      store.metadata(Kind::manifest, snapshot["files"]["other.txt"]["manifest"].get<std::string>());
  const auto chunk = manifest["blocks"][0]["id"].get<std::string>();
  fs::remove(store.root() / "objects/chunk" / chunk.substr(0, 2) / chunk);
  const auto preview = store.plan_reset_lines("main", "code.cpp");
  const auto retained = store.refs();
  require(preview["ready"] == true, "line preview read unrelated payload");
  fails([&] { store.reset_lines("main", "code.cpp", id(preview), {1, 3}); });
  require(store.refs() == retained, "line selection published incomplete closure");
}
void newlines_and_budgets(const fs::path &project) {
  Store store(project / ".syrnck", {}, true);
  const auto code = project / "code.cpp";
  write(code, "old");
  store.commit_directory(project, "main", "missing newline");
  write(code, "new\n");
  auto plan = store.plan_stage_lines(project, "main", "code.cpp");
  auto before = physical(store);
  fails([&] { store.stage_lines(project, "main", "code.cpp", id(plan), {2}); },
        "native line selection joins unterminated lines; select related changes together");
  require(before == physical(store), "unterminated selection wrote objects");
  auto result = store.stage_lines(project, "main", "code.cpp", id(plan), {1, 2});
  auto reset = store.plan_reset_lines("main", "code.cpp");
  before = physical(store);
  fails([&] { store.reset_lines("main", "code.cpp", id(reset), {1}); },
        "native line selection joins unterminated lines; select related changes together");
  require(before == physical(store), "unterminated reset wrote objects");
  result = store.reset_lines("main", "code.cpp", id(reset), {1, 2});
  require(contents(store, result["index"].get<std::string>()) == "old",
          "line reset normalized missing newline");
  store.reset_stage("main");
  write(code, expected(0, "\r\n"));
  store.commit_directory(project, "main", "CRLF");
  write(code, expected(15, "\r\n"));
  plan = store.plan_stage_lines(project, "main", "code.cpp");
  result = store.stage_lines(project, "main", "code.cpp", id(plan), {1, 3});
  require(contents(store, result["index"].get<std::string>()) == expected(5, "\r\n"),
          "line selection normalized CRLF");
  store.reset_stage("main");
  const std::string common = expected(0);
  write(code, std::string(1800, 'a') + '\n' + common);
  store.commit_directory(project, "main", "bounded");
  write(code, common + std::string(1800, 'b') + '\n');
  TextDiffOptions limits;
  limits.max_file_bytes = 2500;
  plan = store.plan_stage_lines(project, "main", "code.cpp", limits);
  before = physical(store);
  fails([&] { store.stage_lines(project, "main", "code.cpp", id(plan), {2}, limits); },
        "native text selection result exceeds file budget");
  require(before == physical(store), "oversized line result wrote objects");
  store.stage_directory(project, "main", {"code.cpp"});
  reset = store.plan_reset_lines("main", "code.cpp", limits);
  before = physical(store);
  fails([&] { store.reset_lines("main", "code.cpp", id(reset), {1}, limits); },
        "native text selection result exceeds file budget");
  require(before == physical(store), "oversized line reset wrote objects");
  for (const auto &[source, target] :
       std::vector<std::pair<std::string, std::string>>{{"", "first\r\nsecond\n"},
                                                        {"first\r\nsecond\n", ""},
                                                        {"alpha\n", "alpha\nfinal"},
                                                        {"a\nx\n", "a\nx"},
                                                        {"utf8 \xc3\xa9\n", "utf8 \xe2\x82\xac\n"}}) {
    store.reset_stage("main");
    write(code, source);
    store.commit_directory(project, "main", "exact line bytes");
    write(code, target);
    plan = store.plan_stage_lines(project, "main", "code.cpp");
    std::vector<std::size_t> all;
    for (std::size_t n = 1; n <= plan["change_count"].get<std::size_t>(); ++n)
      all.push_back(n);
    result = store.stage_lines(project, "main", "code.cpp", id(plan), all);
    require(contents(store, result["index"].get<std::string>()) == target,
            "all line changes differ from target");
    reset = store.plan_reset_lines("main", "code.cpp");
    result = store.reset_lines("main", "code.cpp", id(reset), all);
    require(contents(store, result["index"].get<std::string>()) == source,
            "all line resets differ from baseline");
  }
}
} // namespace
int main() {
  const auto root = fs::temp_directory_path() / ("syrnck-lines-" + hex(random_bytes(16)));
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
          key->fill(61);
        }
        scenario(root / (std::to_string(encrypted) + '-' + std::to_string(layout)), key, layout);
      }
    safety(root / "safety");
    newlines_and_budgets(root / "newlines");
    std::cout << "Verified individual line subsets, atomic retention, exact newlines and cancellation\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
