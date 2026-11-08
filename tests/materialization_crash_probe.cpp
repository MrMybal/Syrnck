#include "native_history_fixture.hpp"
#include <syrnck/workspace.hpp>
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
  Key result; result.fill(80); return result;
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
std::map<std::string, std::string> working(const fs::path &directory) {
  std::map<std::string, std::string> result;
  fs::recursive_directory_iterator at(directory), end;
  for (; at != end; ++at) {
    if (at->path().filename() == ".syrnck") { at.disable_recursion_pending(); continue; }
    if (at->is_regular_file()) result.emplace(utf8(at->path().lexically_relative(directory)), text(read_bytes(at->path())));
  }
  return result;
}
std::optional<std::vector<std::string>> selection(const Json &definition) {
  return definition.is_null() ? std::nullopt : std::optional{definition.get<std::vector<std::string>>()};
}
void select(Store &store, const Json &info) {
  if (info["context"] == "linked") store.use_native_workspace("linked");
}
void prepare(const fs::path &root, bool encrypted, std::string context, std::string direction, bool abort) {
  Fixture f(root / "primary", key(Json{{"encrypted", encrypted}}));
  const auto head = f.revision(Json{{"Source/code", f.file("code")}, {"Content/A/leaf", f.file("leaf")},
      {"Content/B", f.file("hidden")}});
  f.install(head);
  if (context == "linked") f.store.create_native_branch("feature", head);
  const Json target = direction == "grow" ? Json{} : direction == "empty" ? Json::array()
      : direction == "swap" ? Json::array({"Content/A/leaf"}) : Json::array({"Source"});
  atomic_write(root / "fixture.json", bytes(Json{{"encrypted", encrypted}, {"context", context},
      {"direction", direction}, {"abort", abort}, {"head", head}, {"target", target}}.dump()), false);
}
void start(Store &store, const Json &info, OperationOptions options = {}) {
  const auto prefixes = selection(info["target"]);
  const auto plan = plan_native_materialization(store, workspace, prefixes, false, options);
  set_native_materialization(store, workspace, prefixes, info["head"].get<std::string>(), plan["plan"].get<std::string>(), options);
}
void bind(const fs::path &root, Json info) {
  Store store(root / "primary/.syrnck", key(info));
  if (info["context"] == "linked") {
    store.register_native_workspace(root / "primary", "linked", workspace);
    store.use_native_workspace("linked");
    store.switch_directory(workspace, "feature");
  }
  if (info["context"] == "detached") store.detach_checkout(workspace, info["head"].get<std::string>());
  if (info["direction"] != "shrink") {
    auto source = info;
    source["target"] = Json::array({"Source"});
    start(store, source);
  }
  atomic_write(workspace / "local", bytes("unrelated local bytes"), false);
  info["source"] = store.native_materialization()["prefixes"];
  info["before"] = working(workspace);
  Json expected = Json::object();
  if (info["direction"] == "grow" || info["direction"] == "shrink") expected["Source/code"] = "code";
  if (info["direction"] == "grow" || info["direction"] == "swap") expected["Content/A/leaf"] = "leaf";
  if (info["direction"] == "grow") expected["Content/B"] = "hidden";
  expected["local"] = "unrelated local bytes";
  info["after"] = expected;
  info["selection"] = store.native_head();
  store.pack();
  atomic_write(root / "fixture.json", bytes(info.dump()), true);
}
void prime(Store &store, const Json &info) {
  if (!info["abort"].get<bool>()) return;
  std::stop_source stop;
  bool cancelled = false;
  try {
    start(store, info, {stop.get_token(), [&](const ProgressEvent &event) {
      if (event.phase == ProgressPhase::publishing && event.path == "checkout") stop.request_stop();
    }});
  } catch (const Cancelled &) { cancelled = true; }
  require(cancelled && store.checkout_status()["pending"] == true, "materialization abort fixture did not stop");
}
void perform(Store &store, const Json &info) {
  if (store.checkout_status()["pending"].get<bool>()) store.recover_checkout(info["abort"].get<bool>());
  else if (!info["abort"].get<bool>() && store.native_materialization()["prefixes"] != info["target"]) start(store, info);
}
Json verify(Store &store, const Json &info, bool resumed) {
  if (resumed) store.gc(true);
  store.fsck();
  require(store.native_head() == info["selection"], "materialization crash changed HEAD");
  (void)store.closure({{Kind::revision, info["head"].get<std::string>()}}, true);
  const auto status = store.checkout_status();
  const auto policy = store.native_materialization()["prefixes"];
  const bool published = policy == info["target"];
  require(published || policy == info["source"], "materialization crash published an unknown selection");
  if (status["pending"].get<bool>())
    require(status["version"] == 10 && status["layout_version"] == 7 &&
                status["source_materialization"] == info["source"] && status["target_materialization"] == info["target"],
            "materialization crash lost its frozen policy pair");
  else
    require(working(workspace) == info[published ? "after" : "before"].get<std::map<std::string, std::string>>(),
            "materialization crash split the physical files and published selection");
  require(text(read_bytes(workspace / "local")) == "unrelated local bytes", "materialization changed unrelated local data");
  if (info["context"] == "linked")
    require(text(read_bytes(store.root().parent_path() / "Content/B")) == "hidden", "linked materialization changed primary files");
  require(!resumed || (!status["pending"].get<bool>() && published != info["abort"].get<bool>()),
          "materialization resume/abort did not finish correctly");
  return Json{{"verified", true}, {"published", published}};
}
}
namespace syrnck::testing {
void io_checkpoint(std::string_view event, const fs::path &path) {
  if (tracing) {
    const auto type = category(path);
    observations.push_back(Json{{"phase", event}, {"kind", type}, {"occurrence", ++counts[{std::string(event), type}]}});
    return;
  }
  if (!armed || event != phase || category(path) != kind || ++seen != occurrence) return;
  armed = false;
  std::cout << Json{{"phase", phase}, {"kind", kind}, {"occurrence", seen}}.dump() << std::endl;
  std::string line;
  require(static_cast<bool>(std::getline(std::cin, line)) && line == "continue", "materialization checkpoint closed");
}
}
int main(int argc, char **argv) {
  try {
    require(argc >= 3, "materialization probe expects mode and root");
    const auto root = path_from_utf8(argv[2]);
    const std::string mode = argv[1];
    workspace = root / "primary";
    if (mode == "prepare") {
      require(argc == 7, "materialization probe expects encryption, context, direction and recovery");
      prepare(root, std::string_view(argv[3]) == "encrypted", argv[4], argv[5], std::string_view(argv[6]) == "abort");
    } else {
      const auto info = parse_json(read_bytes(root / "fixture.json"));
      if (info["context"] == "linked") workspace = root / "linked";
      if (mode == "bind") bind(root, info);
      else {
        workspace = fs::canonical(workspace);
        Store store(root / "primary/.syrnck", key(info));
        select(store, info);
        if (mode == "trace" || mode == "run") {
          prime(store, info);
          if (mode == "trace") tracing = true;
          else { require(argc == 6, "materialization probe expects phase kind occurrence");
            phase = argv[3]; kind = argv[4]; occurrence = static_cast<std::size_t>(std::stoull(argv[5])); armed = true; }
          perform(store, info);
          if (tracing) std::cout << observations.dump() << '\n';
          else require(!armed, "materialization checkpoint was not reached");
        } else {
          require(mode == "verify" || mode == "resume", "unknown materialization probe mode");
          if (mode == "resume") perform(store, info);
          std::cout << verify(store, info, mode == "resume").dump() << '\n';
        }
      }
    }
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
