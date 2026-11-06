#include "native_history_fixture.hpp"
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
void prepare(const fs::path &root, bool encrypted, bool abort) {
  const Json info_key{{"encrypted", encrypted}};
  Fixture f(root / "source", key(info_key));
  const auto target = f.revision(
      Json{{"a", f.file("target a")}, {"missing", f.file("restored")}, {"shape", f.file("target flat")}});
  const auto head = f.revision(Json{{"a", f.file("head a")},
                                    {"missing", f.file("head missing")},
                                    {"shape/child", f.file("head child")}},
                               Json::array({target}));
  f.install(head);
  f.store.set_ref("pins/target", {Kind::revision, target});
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
void start(Store &store, const Json &info, OperationOptions options = {}) {
  const auto plan = plan_native_hard_reset(store, workspace, "main", info["target"].get<std::string>());
  native_hard_reset(store, workspace, "main", info["head"].get<std::string>(),
                    info["target"].get<std::string>(), plan["plan"].get<std::string>(), options);
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
  require(cancelled && store.checkout_status()["pending"] == true &&
              store.refs().at("heads/main").id == info["head"].get<std::string>(),
          "hard reset abort fixture did not stop before publication");
}
void perform(const fs::path &root, const Json &info) {
  Store store(root / "source/.syrnck", key(info));
  if (store.checkout_status()["pending"].get<bool>())
    store.recover_checkout(info["abort"].get<bool>());
  else if (!info["abort"].get<bool>() && store.refs().at("heads/main").id == info["head"].get<std::string>())
    start(store, info);
}
Json verify(const fs::path &root, const Json &info, bool resumed) {
  Store store(root / "source/.syrnck", key(info));
  if (resumed)
    store.gc(true);
  store.fsck();
  const auto refs = store.refs();
  const auto status = store.checkout_status();
  const auto head = refs.at("heads/main").id;
  const bool published = head == info["target"].get<std::string>();
  require(published || head == info["head"].get<std::string>(), "hard reset crash published an unknown head");
  const auto index = refs.find("staging/main");
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
      if (name.starts_with("archive/checkout/")) {
        validate_recovery(object.id);
        archived = true;
      }
    require(archived, "hard reset published without recovery archive");
  }
  (void)store.closure(
      {{Kind::revision, info["head"].get<std::string>()}, {Kind::revision, info["index"].get<std::string>()}},
      true);
  require(text(read_bytes(workspace / "outside")) == "unrelated work", "hard reset modified unrelated work");
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
      require(argc == 5, "hard reset probe expects encryption and action");
      prepare(root, std::string_view(argv[3]) == "encrypted", std::string_view(argv[4]) == "abort");
    } else {
      workspace = fs::canonical(workspace);
      const auto info = parse_json(read_bytes(root / "fixture.json"));
      if (mode == "run" || mode == "trace") {
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
