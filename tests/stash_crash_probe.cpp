#include "native_history_fixture.hpp"
#include "native_target.hpp"
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
void prepare(const fs::path &root, bool encrypted, std::string action, bool abort, bool sparse) {
  Fixture f(root / "source", key(Json{{"encrypted", encrypted}}));
  auto files = Json{{"a", f.file("base a")}, {"missing", f.file("base missing")}, {"shape", f.file("base flat")}};
  if (sparse) {
    files["Content/A/leaf"] = f.file("base leaf");
    files["Content/B"] = f.file("base hidden");
  }
  const auto head = f.revision(files);
  f.install(head);
  if (sparse) {
    f.store.create_native_branch("feature", head);
    atomic_write(root / "fixture.json", bytes(Json{{"encrypted", encrypted}, {"action", action},
        {"abort", abort}, {"sparse", true}, {"head", head}}.dump()), false);
    return;
  }
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
std::string branch(const Json &info) { return info.value("sparse", false) ? "feature" : "main"; }
void select_context(Store &store, const Json &info) {
  if (info.value("sparse", false)) store.use_native_workspace("linked");
}
void bind_partial(const fs::path &root, Json info) {
  Store store(root / "source/.syrnck", key(info));
  store.register_partial_native_workspace(root / "source", "linked", workspace,
      {"a", "added", "missing", "shape", "Content/A/leaf"});
  store.use_native_workspace("linked");
  store.switch_directory(workspace, "feature");
  const auto write = [&](std::string_view name, std::string_view value) {
    const auto path = workspace / path_from_utf8(name);
    fs::create_directories(path.parent_path());
    atomic_write(path, bytes(value), true);
  };
  write("outside", "excluded local data");
  info["clean"] = working(workspace);
  write("a", "staged a");
  write("added", "staged added");
  const auto initial = store.stage_directory(workspace, "feature", {"a", "added"});
  auto staged = store.metadata(Kind::revision, initial);
  std::istringstream input("staged hidden");
  staged["files"]["Content/B"]["manifest"] = store.ingest(input).first;
  const auto index = store.put(Kind::revision, bytes(staged.dump())).first;
  store.set_ref(native_staging_reference(store, "feature"), index);
  info["index"] = index.id;
  write("a", "working a");
  write("added", "working added");
  fs::remove(workspace / "missing");
  fs::remove(workspace / "shape");
  write("shape/child", "working child");
  fs::remove_all(workspace / "Content");
  write("Content", "physical ancestor obstruction");
  info["dirty"] = working(workspace);
  info["captured"] = info["dirty"];
  info["captured"].erase("outside");
  info["stash"] = Json{};
  if (info["action"] != "save")
    info["stash"] = NativeStash(store).save(workspace, "feature", "crash fixture", {true})["stash"]["id"];
  store.pack();
  atomic_write(root / "fixture.json", bytes(info.dump()), true);
}
void start(Store &store, const Json &info, OperationOptions options = {}) {
  NativeStash stash(store);
  if (info["action"] == "save")
    stash.save(workspace, branch(info), "crash fixture", {true}, options);
  else {
    NativeStashApplyOptions settings;
    settings.restore_index = true;
    const auto id = info["stash"].get<std::string>();
    const auto plan = stash.plan_apply(workspace, branch(info), id, settings, false, options);
    stash.apply(workspace, branch(info), id, plan["plan"].get<std::string>(), info["action"] == "pop", settings,
                options);
  }
}
void prime_abort(const fs::path &root, const Json &info) {
  if (!info["abort"].get<bool>())
    return;
  Store store(root / "source/.syrnck", key(info));
  select_context(store, info);
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
  select_context(store, info);
  if (store.checkout_status()["pending"].get<bool>())
    store.recover_checkout(info["abort"].get<bool>());
  else if (!info["abort"].get<bool>()) {
    const bool saving = info["action"] == "save";
    if ((saving && NativeStash(store).list()["total"] == 0) ||
        (!saving && !store.refs().contains(native_staging_reference(store, branch(info)))))
      start(store, info);
  }
}
Json verify(const fs::path &root, const Json &info, bool resumed) {
  Store store(root / "source/.syrnck", key(info));
  select_context(store, info);
  if (resumed)
    store.gc(true);
  store.fsck();
  const auto refs = store.refs();
  require(refs.at("heads/" + branch(info)).id == info["head"].get<std::string>(), "stash crash moved branch head");
  const auto status = store.checkout_status();
  const bool pending = status["pending"].get<bool>();
  const bool saving = info["action"] == "save", pop = info["action"] == "pop";
  const auto slot = native_staging_reference(store, branch(info));
  const bool indexed = refs.contains(slot);
  const bool published = saving ? !indexed : indexed;
  if (saving && indexed)
    require(refs.at(slot).id == info["index"].get<std::string>(),
            "stash save changed index before publication");
  NativeStash stash(store);
  const auto records = stash.list();
  if (!saving)
    require(records["total"] == (pop && published ? 0 : 1), "stash pop/index publication split");
  const auto validate = [&](const Json &entry) {
    require(entry["base"] == info["head"] && entry["index"] == info["index"],
            "stash lost original base/index");
    const auto value = store.metadata(Kind::revision, entry["work"].get<std::string>());
    const auto &captured = info.contains("captured") ? info["captured"] : info["dirty"];
    require(value["files"].size() == captured.size(), "stash lost working paths");
    for (const auto &[path, file] : value["files"].items()) {
      std::ostringstream output;
      store.reconstruct(file["manifest"].get<std::string>(), output);
      require(captured.contains(path) && output.str() == captured[path].get<std::string>(),
              "stash lost working bytes");
    }
    (void)store.closure({{Kind::revision, entry["work"].get<std::string>()}}, true);
    if (info.value("sparse", false))
      require(entry["version"] == 3 && entry["materialization"] == store.native_materialization()["prefixes"],
              "stash lost its frozen partial selection");
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
    const auto staged = store.metadata(Kind::revision, refs.at(slot).id);
    require(staged["files"] == store.metadata(Kind::revision, info["index"].get<std::string>())["files"],
            "stash restored working bytes instead of frozen index");
  }
  if (info.value("sparse", false))
    require(refs.at("heads/main").id == info["head"].get<std::string>() &&
                text(read_bytes(root / "source/Content/B")) == "base hidden" &&
                text(read_bytes(workspace / "outside")) == "excluded local data",
            "partial stash changed another workspace or excluded local data");
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
      require(argc == 6 || argc == 7, "stash probe expects encryption, action and recovery");
      prepare(root, std::string_view(argv[3]) == "encrypted", argv[4], std::string_view(argv[5]) == "abort", argc == 7);
    } else {
      const auto info = parse_json(read_bytes(root / "fixture.json"));
      if (info.value("sparse", false)) workspace = root / "linked";
      if (mode == "bind") {
        if (info.value("sparse", false)) bind_partial(root, info);
      } else if (mode == "trace" || mode == "run") {
        workspace = fs::canonical(workspace);
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
