#include "native_history_fixture.hpp"
#include "native_target.hpp"
#include <syrnck/workspace.hpp>
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
  result.fill(75);
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
void prepare(const fs::path &root, bool encrypted, bool abort, bool sparse) {
  const Json info_key{{"encrypted", encrypted}};
  Fixture f(root / "source", key(info_key));
  auto target_files = Json{{"a", f.file("target a")}, {"missing", f.file("restored")}, {"shape", f.file("target flat")}};
  auto head_files = Json{{"a", f.file("head a")},
                                    {"missing", f.file("head missing")},
                                    {"shape/child", f.file("head child")}};
  if (sparse) {
    target_files["Content/A/leaf"] = f.file("target leaf");
    target_files["Content/B"] = f.file("target hidden");
    head_files["Content/A/leaf"] = f.file("head leaf");
    head_files["Content/B"] = f.file("head hidden");
  }
  const auto target = f.revision(target_files);
  const auto head = f.revision(head_files, Json::array({target}));
  f.install(head);
  f.store.set_ref("pins/target", {Kind::revision, target});
  if (sparse) {
    f.store.create_native_branch("feature", head);
    atomic_write(root / "fixture.json", bytes(Json{{"encrypted", encrypted}, {"abort", abort},
        {"sparse", true}, {"head", head}, {"target", target}}.dump()), false);
    return;
  }
  f.write("a", "staged a");
  f.write("new", "staged new");
  const auto index = f.store.stage_directory(f.project, "main", {"a", "new"});
  f.write("a", "dirty a");
  f.write("new", "dirty new");
  f.write("shape/untracked", "obstructing untracked work");
  f.write("outside", "unrelated work");
  fs::create_directories(f.project / "shape/empty/deeper");
  fs::remove(f.project / "missing");
  f.store.pack();
  const auto plan = plan_native_hard_reset(f.store, f.project, "main", target);
  require(plan["ready"] == true, "hard reset crash fixture is blocked");
  const Json info{{"encrypted", encrypted},
                  {"abort", abort},
                  {"head", head},
                  {"target", target},
                  {"index", index},
                  {"plan", plan["plan"]},
                  {"working", working(f.project)},
                  {"target_files", Json{{"a", "target a"},
                                        {"missing", "restored"},
                                        {"shape", "target flat"},
                                        {"outside", "unrelated work"}}}};
  atomic_write(root / "fixture.json", bytes(info.dump()), false);
}
void bind_partial(const fs::path &root, Json info) {
  Store store(root / "source/.syrnck", key(info));
  store.register_partial_native_workspace(root / "source", "linked", workspace,
      {"a", "new", "missing", "shape", "Content/A/leaf"});
  store.use_native_workspace("linked");
  store.switch_directory(workspace, "feature");
  const auto write = [&](std::string_view name, std::string_view value) {
    const auto path = workspace / path_from_utf8(name);
    fs::create_directories(path.parent_path());
    atomic_write(path, bytes(value), true);
  };
  write("a", "staged a");
  write("new", "staged new");
  info["index"] = store.stage_directory(workspace, "feature", {"a", "new"});
  write("a", "dirty a");
  write("new", "dirty new");
  write("shape/untracked", "obstructing untracked work");
  write("outside", "unrelated work");
  fs::create_directories(workspace / "shape/empty/deeper");
  fs::remove(workspace / "missing");
  fs::remove_all(workspace / "Content");
  write("Content", "physical ancestor obstruction");
  store.pack();
  const auto plan = plan_native_hard_reset(store, workspace, "feature", info["target"].get<std::string>());
  require(plan["ready"] == true && plan["version"] == 2, "partial hard reset crash fixture is blocked");
  info["working"] = working(workspace);
  info["target_files"] = Json{{"a", "target a"}, {"missing", "restored"},
      {"shape", "target flat"}, {"Content/A/leaf", "target leaf"}, {"outside", "unrelated work"}};
  atomic_write(root / "fixture.json", bytes(info.dump()), true);
}
std::string branch(const Json &info) { return info.value("sparse", false) ? "feature" : "main"; }
void select_context(Store &store, const Json &info) {
  if (info.value("sparse", false)) store.use_native_workspace("linked");
}
void start(Store &store, const Json &info, OperationOptions options = {}) {
  const auto plan = plan_native_hard_reset(store, workspace, branch(info), info["target"].get<std::string>());
  native_hard_reset(store, workspace, branch(info), info["head"].get<std::string>(),
                    info["target"].get<std::string>(), plan["plan"].get<std::string>(), options);
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
  require(cancelled && store.checkout_status()["pending"] == true &&
              store.refs().at("heads/" + branch(info)).id == info["head"].get<std::string>(),
          "hard reset abort fixture did not stop before publication");
}
void perform(const fs::path &root, const Json &info) {
  Store store(root / "source/.syrnck", key(info));
  select_context(store, info);
  if (store.checkout_status()["pending"].get<bool>())
    store.recover_checkout(info["abort"].get<bool>());
  else if (!info["abort"].get<bool>() && store.refs().at("heads/" + branch(info)).id == info["head"].get<std::string>())
    start(store, info);
}
Json verify(const fs::path &root, const Json &info, bool resumed) {
  Store store(root / "source/.syrnck", key(info));
  select_context(store, info);
  if (resumed)
    store.gc(true);
  store.fsck();
  const auto refs = store.refs();
  const auto status = store.checkout_status();
  const auto head = refs.at("heads/" + branch(info)).id;
  const bool published = head == info["target"].get<std::string>();
  require(published || head == info["head"].get<std::string>(), "hard reset crash published an unknown head");
  const auto index = refs.find(native_staging_reference(store, branch(info)));
  require(published ? index == refs.end()
                    : index != refs.end() && index->second.id == info["index"].get<std::string>(),
          "hard reset crash split head and index publication");
  const auto validate_recovery = [&](std::string_view id) {
    const auto revision = store.metadata(Kind::revision, id);
    require(revision["parents"] == Json::array({info["head"], info["index"]}),
            "hard reset lost head/index ancestry");
    for (const auto &[path, file] : revision["files"].items()) {
      require(info["working"].contains(path), "hard reset captured an unrelated path");
      std::ostringstream output;
      store.reconstruct(file["manifest"].get<std::string>(), output);
      require(output.str() == info["working"][path].get<std::string>(),
              "hard reset lost pre-reset working bytes");
    }
    require(revision["files"].size() + 1 == info["working"].size(),
            "hard reset recovery omitted a working file");
  };
  if (status["pending"].get<bool>())
    validate_recovery(status["source"].get<std::string>());
  if (published) {
    bool archived = false;
    for (const auto &[name, object] : refs)
      if (name.starts_with("archive/checkout/") &&
          store.metadata(Kind::revision, object.id)["message"] ==
              "Syrnck hard reset recovery v1: " + branch(info)) {
        validate_recovery(object.id);
        archived = true;
      }
    require(archived, "hard reset published without recovery archive");
  }
  (void)store.closure(
      {{Kind::revision, info["head"].get<std::string>()}, {Kind::revision, info["index"].get<std::string>()}},
      true);
  require(text(read_bytes(workspace / "outside")) == "unrelated work", "hard reset modified unrelated work");
  if (info.value("sparse", false)) {
    require(refs.at("heads/main").id == info["head"].get<std::string>() &&
                text(read_bytes(root / "source/Content/B")) == "head hidden",
            "partial hard reset changed another workspace");
    const auto old = store.metadata(Kind::revision, info["index"].get<std::string>());
    require(old["files"].contains("Content/B"), "partial hard reset lost hidden staged entries");
    if (status["pending"].get<bool>())
      require(status["version"] == 9 && status["layout_version"] == 5,
              "partial hard reset did not persist the frozen projection");
  }
  if (!status["pending"].get<bool>()) {
    const auto expected = published ? info["target_files"] : info["working"];
    require(working(workspace) == expected.get<std::map<std::string, std::string>>(),
            "hard reset crash left an unjournaled partial workspace");
    if (!published)
      require(fs::is_directory(workspace / "shape/empty/deeper"), "hard reset abort lost empty directories");
  }
  require(!resumed || (!status["pending"].get<bool>() && published != info["abort"].get<bool>()),
          "hard reset did not resume or abort correctly");
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
  require(static_cast<bool>(std::getline(std::cin, line)) && line == "continue",
          "hard reset checkpoint closed");
}
} // namespace syrnck::testing
int main(int argc, char **argv) {
  try {
    require(argc >= 3, "hard reset probe expects mode and root");
    const auto root = path_from_utf8(argv[2]);
    workspace = root / "source";
    const std::string mode = argv[1];
    if (mode == "prepare") {
      require(argc == 5 || argc == 6, "hard reset probe expects encryption and action");
      prepare(root, std::string_view(argv[3]) == "encrypted", std::string_view(argv[4]) == "abort", argc == 6);
    } else {
      const auto info = parse_json(read_bytes(root / "fixture.json"));
      if (info.value("sparse", false)) workspace = root / "linked";
      if (mode == "bind") {
        if (info.value("sparse", false)) bind_partial(root, info);
      } else if (mode == "run" || mode == "trace") {
        workspace = fs::canonical(workspace);
        prime_abort(root, info);
        if (mode == "trace")
          tracing = true;
        else {
          require(argc == 6, "hard reset probe expects phase kind and occurrence");
          wanted_phase = argv[3];
          wanted_kind = argv[4];
          wanted_count = static_cast<std::size_t>(std::stoull(argv[5]));
          armed = true;
        }
        perform(root, info);
        if (mode == "trace")
          std::cout << observations.dump() << '\n';
        else
          require(!armed, "hard reset checkpoint was not reached");
      } else {
        require(mode == "verify" || mode == "resume", "unknown hard reset probe mode");
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
