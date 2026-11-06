#include "native_history_fixture.hpp"
#include <syrnck/stash.hpp>
#include <iostream>
using namespace native_test;
namespace {
bool armed = false, tracing = false;
std::string wanted_phase, wanted_kind;
std::size_t wanted_count = 0, seen = 0;
Json observations = Json::array();
std::map<std::pair<std::string, std::string>, std::size_t> counts;
fs::path workspace;
std::optional<Key> key(const Json &info) {
  if (!info["encrypted"].get<bool>())
    return {};
  Key result;
  result.fill(77);
  return result;
}
std::string category(const fs::path &path) {
  const auto name = utf8(path.filename());
  if (name == "refs.json")
    return "refs";
  if (name == "checkout.state")
    return "checkout";
  const auto parent = utf8(path.parent_path().filename());
  if (parent == "new" || parent == "old")
    return parent;
  const auto relative = path.lexically_relative(workspace);
  if (!relative.empty() && *relative.begin() != ".syrnck" && *relative.begin() != "..")
    return "workspace";
  return utf8(path.parent_path().parent_path().filename());
}
std::map<std::string, std::string> working(const fs::path &directory) {
  std::map<std::string, std::string> files;
  fs::recursive_directory_iterator at(directory), end;
  for (; at != end; ++at) {
    if (at->path().filename() == ".syrnck") {
      at.disable_recursion_pending();
      continue;
    }
    if (at->is_regular_file())
      files.emplace(utf8(at->path().lexically_relative(directory)), text(read_bytes(at->path())));
  }
  return files;
}
void prepare(const fs::path &root, bool encrypted, std::string action, bool abort) {
  Fixture f(root / "source", key(Json{{"encrypted", encrypted}}));
  const auto head = f.revision(
      Json{{"a", f.file("base a")}, {"missing", f.file("base missing")}, {"shape", f.file("base flat")}});
  f.install(head);
  const auto clean = working(f.project);
  f.write("a", "staged a");
  f.write("added", "staged added");
  const auto index = f.store.stage_directory(f.project, "main", {"a", "added"});
  f.write("a", "working a");
  f.write("added", "working added");
  fs::remove(f.project / "missing");
  fs::remove(f.project / "shape");
  f.write("shape/child", "working child");
  f.write("outside", "working untracked");
  const auto dirty = working(f.project);
  Json id;
  if (action != "save")
    id = NativeStash(f.store).save(f.project, "main", "crash fixture", {true})["stash"]["id"];
  f.store.pack();
  atomic_write(root / "fixture.json",
               bytes(Json{{"encrypted", encrypted},
                          {"action", action},
                          {"abort", abort},
                          {"head", head},
                          {"index", index},
                          {"stash", id},
                          {"clean", clean},
                          {"dirty", dirty}}
                         .dump()),
               false);
}
void start(Store &store, const Json &info, OperationOptions options = {}) {
  NativeStash stash(store);
  if (info["action"] == "save")
    stash.save(workspace, "main", "crash fixture", {true}, options);
  else {
    NativeStashApplyOptions settings;
    settings.restore_index = true;
    const auto id = info["stash"].get<std::string>();
    const auto plan = stash.plan_apply(workspace, "main", id, settings, false, options);
    stash.apply(workspace, "main", id, plan["plan"].get<std::string>(), info["action"] == "pop", settings,
                options);
  }
}
void prime_abort(const fs::path &root, const Json &info) {
  if (!info["abort"].get<bool>())
    return;
  Store store(root / "source/.syrnck", key(info));
  std::stop_source stop;
  bool cancelled = false;
  try {
    start(store, info, {stop.get_token(), [&](const ProgressEvent &event) {
                          if (event.phase == ProgressPhase::publishing && event.path == "checkout")
                            stop.request_stop();
                        }});
  } catch (const Cancelled &) {
    cancelled = true;
  }
  require(cancelled && store.checkout_status()["pending"] == true,
          "stash abort fixture did not stop before publication");
}
void perform(const fs::path &root, const Json &info) {
  Store store(root / "source/.syrnck", key(info));
  if (store.checkout_status()["pending"].get<bool>())
    store.recover_checkout(info["abort"].get<bool>());
  else if (!info["abort"].get<bool>()) {
    const bool saving = info["action"] == "save";
    if ((saving && NativeStash(store).list()["total"] == 0) ||
        (!saving && !store.refs().contains("staging/main")))
      start(store, info);
  }
}
Json verify(const fs::path &root, const Json &info, bool resumed) {
  Store store(root / "source/.syrnck", key(info));
  if (resumed)
    store.gc(true);
  store.fsck();
  const auto refs = store.refs();
  require(refs.at("heads/main").id == info["head"].get<std::string>(), "stash crash moved branch head");
  const auto status = store.checkout_status();
  const bool pending = status["pending"].get<bool>();
  const bool saving = info["action"] == "save", pop = info["action"] == "pop";
  const bool indexed = refs.contains("staging/main");
  const bool published = saving ? !indexed : indexed;
  if (saving && indexed)
    require(refs.at("staging/main").id == info["index"].get<std::string>(),
            "stash save changed index before publication");
  NativeStash stash(store);
  const auto records = stash.list();
  if (!saving)
    require(records["total"] == (pop && published ? 0 : 1), "stash pop/index publication split");
  const auto validate = [&](const Json &entry) {
    require(entry["base"] == info["head"] && entry["index"] == info["index"],
            "stash lost original base/index");
    const auto value = store.metadata(Kind::revision, entry["work"].get<std::string>());
    require(value["files"].size() == info["dirty"].size(), "stash lost working paths");
    for (const auto &[path, file] : value["files"].items()) {
      std::ostringstream output;
      store.reconstruct(file["manifest"].get<std::string>(), output);
      require(info["dirty"].contains(path) && output.str() == info["dirty"][path].get<std::string>(),
              "stash lost working bytes");
    }
    (void)store.closure({{Kind::revision, entry["work"].get<std::string>()}}, true);
  };
  for (const auto &entry : records["stashes"])
    validate(stash.show(entry["id"].get<std::string>(), true));
  if (pop && published)
    validate(stash.archived(info["stash"].get<std::string>(), true));
  if (saving && (pending || published || info["abort"].get<bool>()))
    require(records["total"] == 1, "stash cleaning began without retained record");
  if (!pending) {
    const bool dirty = saving ? !published : published;
    require(working(workspace) == info[dirty ? "dirty" : "clean"].get<std::map<std::string, std::string>>(),
            "stash crash left unjournaled partial files");
  }
  if (published && !saving) {
    const auto staged = store.metadata(Kind::revision, refs.at("staging/main").id);
    require(staged["files"] == store.metadata(Kind::revision, info["index"].get<std::string>())["files"],
            "stash restored working bytes instead of frozen index");
  }
  require(!resumed || (!pending && published != info["abort"].get<bool>()),
          "stash did not resume or abort correctly");
  return Json{{"verified", true}, {"published", published}};
}
} // namespace
namespace syrnck::testing {
void io_checkpoint(std::string_view phase, const fs::path &target) {
  if (tracing) {
    const auto kind = category(target);
    const auto count = ++counts[{std::string(phase), kind}];
    observations.push_back(Json{{"phase", phase}, {"kind", kind}, {"occurrence", count}});
    return;
  }
  if (!armed || phase != wanted_phase || category(target) != wanted_kind || ++seen != wanted_count)
    return;
  armed = false;
  std::cout << Json{{"phase", phase}, {"kind", wanted_kind}, {"occurrence", seen}}.dump() << std::endl;
  std::string line;
  require(static_cast<bool>(std::getline(std::cin, line)) && line == "continue", "stash checkpoint closed");
}
} // namespace syrnck::testing
int main(int argc, char **argv) {
  try {
    require(argc >= 3, "stash probe expects mode and root");
    const auto root = path_from_utf8(argv[2]);
    workspace = root / "source";
    const std::string mode = argv[1];
    if (mode == "prepare") {
      require(argc == 6, "stash probe expects encryption, action and recovery");
      prepare(root, std::string_view(argv[3]) == "encrypted", argv[4], std::string_view(argv[5]) == "abort");
    } else {
      workspace = fs::canonical(workspace);
      const auto info = parse_json(read_bytes(root / "fixture.json"));
      if (mode == "trace" || mode == "run") {
        prime_abort(root, info);
        if (mode == "trace")
          tracing = true;
        else {
          require(argc == 6, "stash probe expects phase kind and occurrence");
          wanted_phase = argv[3];
          wanted_kind = argv[4];
          wanted_count = static_cast<std::size_t>(std::stoull(argv[5]));
          armed = true;
        }
        perform(root, info);
        if (mode == "trace")
          std::cout << observations.dump() << '\n';
        else
          require(!armed, "stash checkpoint was not reached");
      } else {
        require(mode == "verify" || mode == "resume", "unknown stash probe mode");
        if (mode == "resume")
          perform(root, info);
        std::cout << verify(root, info, mode == "resume").dump() << '\n';
      }
    }
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
