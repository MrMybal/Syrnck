#include "native_history_fixture.hpp"
#include "native_materialization.hpp"
#include <iostream>
using namespace native_test;
namespace {
bool armed = false, tracing = false;
std::string phase, kind;
std::size_t occurrence = 0, seen = 0;
Json observations = Json::array();
std::map<std::pair<std::string, std::string>, std::size_t> counts;
fs::path workspace;
std::optional<Key> key(const Json &info) {
  if (!info["encrypted"].get<bool>()) return {};
  Key result; result.fill(78); return result;
}
std::string category(const fs::path &path) {
  const auto name = utf8(path.filename());
  if (name == "refs.json") return "refs";
  if (name == "config.json") return "config";
  if (name == "checkout.state") return "checkout";
  const auto parent = utf8(path.parent_path().filename());
  if (parent == "new" || parent == "old") return parent;
  const auto relative = path.lexically_relative(workspace);
  if (!relative.empty() && *relative.begin() != ".syrnck" && *relative.begin() != "..") return "workspace";
  return utf8(path.parent_path().parent_path().filename());
}
void prepare(const fs::path &root, bool encrypted, std::string action, bool abort, bool linked, bool sparse) {
  Fixture f(root / "source", key(Json{{"encrypted", encrypted}}));
  const auto a = f.revision(Json{{"asset", f.file("old asset")}, {"shape", f.file("flat")}});
  const auto b = f.revision(Json{{"asset", f.file("main asset")}, {"shape/child", f.file("nested")}}, Json::array({a}));
  const auto old = f.revision(Json{{"asset", f.file("inactive detached history")}});
  f.install(b);
  if (linked) f.store.create_native_branch("feature", b);
  f.store.set_ref("workspace/detached", {Kind::revision, old});
  f.store.pack();
  atomic_write(root / "fixture.json", bytes(Json{{"encrypted", encrypted}, {"action", action},
      {"abort", abort}, {"linked", linked}, {"sparse", sparse}, {"a", a}, {"b", b}, {"old", old}}.dump()), false);
}
void registration(Store &store, const fs::path &primary, const Json &info, OperationOptions options = {}) {
  if (info.value("sparse", false)) store.register_partial_native_workspace(primary, "linked", workspace, {"shape"}, options);
  else store.register_native_workspace(primary, "linked", workspace, options);
}
void start(Store &store, const Json &info, OperationOptions options = {}) {
  if (info["action"] == "register") registration(store, store.root().parent_path(), info, options);
  else if (info["action"] == "leave") store.switch_directory(workspace, info.value("linked", false) ? "feature" : "main", options);
  else store.detach_checkout(workspace, info[info["action"] == "enter" ? "a" : "b"].get<std::string>(), options);
}
bool finished(const Store &store, const Json &info) {
  if (info["action"] == "register") return store.refs().contains("workspaces/linked/registration");
  const auto head = store.native_head();
  return info["action"] == "leave" ? head["branch"] == (info.value("linked", false) ? "feature" : "main")
      : head["branch"].is_null() && head["revision"] == info[info["action"] == "enter" ? "a" : "b"];
}
void prime_abort(Store &store, const Json &info) {
  if (!info["abort"].get<bool>()) return;
  std::stop_source stop;
  bool cancelled = false;
  try {
    start(store, info, {stop.get_token(), [&](const ProgressEvent &event) {
      if (event.phase == ProgressPhase::publishing && event.path == "checkout") stop.request_stop();
    }});
  } catch (const Cancelled &) { cancelled = true; }
  require(cancelled && store.checkout_status()["pending"] == true, "detached abort fixture did not stop");
}
void perform(Store &store, const Json &info) {
  if (store.checkout_status()["pending"].get<bool>()) store.recover_checkout(info["abort"].get<bool>());
  else if (!info["abort"].get<bool>() && !finished(store, info)) start(store, info);
}
Json verify(Store &store, const Json &info, bool resumed) {
  if (resumed) store.gc(true);
  store.fsck();
  require(store.refs().at("heads/main").id == info["b"].get<std::string>(), "detached crash moved main");
  if (info.value("linked", false))
    require(store.refs().at("heads/feature").id == info["b"].get<std::string>(), "linked crash moved feature");
  if (info["action"] == "register") {
    const auto refs = store.refs();
    const auto registered = refs.contains("workspaces/linked/registration");
    require(refs.contains("workspaces/linked/workspace/HEAD") == registered &&
                refs.contains("workspaces/linked/workspace/detached") == registered &&
                refs.contains("workspace/HEAD") == registered, "workspace registration published partial roots");
    if (info.value("sparse", false))
      require(refs.contains("workspaces/linked/workspace/materialization") == registered,
              "partial registration published a detached policy or unmasked context");
    if (registered) {
      store.use_native_workspace("linked");
      require(store.native_head()["branch"].is_null() && store.native_head()["revision"].is_string() &&
                  !fs::exists(workspace / "asset"), "registered workspace selected or changed primary files");
      store.use_native_workspace("");
    }
    require(!resumed || registered, "workspace registration did not resume");
    return Json{{"verified", true}};
  }
  const auto head = store.native_head();
  const auto pending = store.checkout_status()["pending"].get<bool>();
  if (!pending) {
    const auto tree = store.metadata(Kind::revision, head["revision"].get<std::string>());
    const auto policy = read_native_materialization(store).policy;
    for (const auto &[path, entry] : tree["files"].items()) {
      if (!policy.includes(path)) {
        require(!fs::exists(workspace / path_from_utf8(path)), "partial crash materialized excluded files");
        continue;
      }
      std::ostringstream output;
      store.reconstruct(entry["manifest"].get<std::string>(), output);
      require(text(read_bytes(workspace / path_from_utf8(path))) == output.str(), "detached crash left partial files");
    }
    require(fs::is_directory(workspace / "shape") == (head["revision"] == info["b"]), "detached crash left wrong namespace");
  }
  // Every original history is reconstructible, including replaced inactive detached data.
  for (const auto name : {"a", "b", "old"}) {
    const auto id = info[name].get<std::string>();
    (void)store.closure({{Kind::revision, id}}, true);
  }
  if (resumed) {
    require(!pending && finished(store, info) != info["abort"].get<bool>(), "detached resume/abort selected wrong HEAD");
    require(store.refs().at("archive/" + info["old"].get<std::string>()).id == info["old"].get<std::string>(),
            "detached replacement discarded old private history");
  }
  return Json{{"verified", true}};
}
} // namespace
namespace syrnck::testing {
void io_checkpoint(std::string_view at, const fs::path &target) {
  const auto type = category(target);
  if (tracing) {
    const auto count = ++counts[{std::string(at), type}];
    observations.push_back(Json{{"phase", at}, {"kind", type}, {"occurrence", count}});
  } else if (armed && at == phase && type == kind && ++seen == occurrence) {
    armed = false;
    std::cout << Json{{"phase", at}, {"kind", type}, {"occurrence", seen}}.dump() << std::endl;
    std::string reply;
    require(static_cast<bool>(std::getline(std::cin, reply)) && reply == "continue", "detached checkpoint closed");
  }
}
} // namespace syrnck::testing
int main(int argc, char **argv) {
  try {
    require(argc >= 3, "detached probe expects mode and root");
    const auto root = path_from_utf8(argv[2]);
    workspace = root / "source";
    const std::string mode = argv[1];
    if (mode == "prepare") {
      require(argc == 6 || argc == 7, "detached probe expects encryption, action and recovery");
      prepare(root, std::string_view(argv[3]) == "encrypted", argv[4], std::string_view(argv[5]) == "abort", argc == 7,
              argc == 7 && std::string_view(argv[6]) == "sparse");
    } else {
      const auto info = parse_json(read_bytes(root / "fixture.json"));
      const auto primary = fs::canonical(workspace);
      Store store(primary / ".syrnck", key(info));
      if (info.value("linked", false)) {
        workspace = root / "linked";
        if (mode == "bind" && info["action"] != "register") {
          registration(store, primary, info);
          store.use_native_workspace("linked");
          workspace = fs::canonical(workspace);
          store.switch_directory(workspace, "feature");
          store.set_ref("workspaces/linked/workspace/detached", {Kind::revision, info["old"].get<std::string>()});
        } else if (mode != "bind" && info["action"] != "register") {
          store.use_native_workspace("linked");
          workspace = fs::canonical(workspace);
        }
      } else workspace = primary;
      if (mode == "bind") {
        // Fixture setup runs after cloning, so machine-bound HEAD has the real path.
        if (info["action"] != "enter" && info["action"] != "register") store.detach_checkout(workspace, info["a"].get<std::string>());
      } else if (mode == "trace" || mode == "run") {
        prime_abort(store, info);
        if (mode == "trace") tracing = true;
        else {
          require(argc == 6, "detached probe expects phase kind occurrence");
          phase = argv[3]; kind = argv[4]; occurrence = static_cast<std::size_t>(std::stoull(argv[5])); armed = true;
        }
        perform(store, info);
        if (tracing) std::cout << observations.dump() << '\n';
        else require(!armed, "detached checkpoint was not reached");
      } else {
        require(mode == "verify" || mode == "resume", "unknown detached probe mode");
        if (mode == "resume") perform(store, info);
        std::cout << verify(store, info, mode == "resume").dump() << '\n';
      }
    }
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
