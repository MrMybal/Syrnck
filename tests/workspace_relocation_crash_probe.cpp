#include "native_history_fixture.hpp"
#include "native_directory_identity.hpp"
#include <syrnck/workspace.hpp>
#include <iostream>
using namespace native_test;
namespace {
bool armed = false, tracing = false;
std::string checkpoint, kind;
std::size_t occurrence = 0, seen = 0;
Json events = Json::array();
std::map<std::pair<std::string, std::string>, std::size_t> counts;
fs::path fixture_root;
std::optional<Key> key(const Json &info) { if (!info["encrypted"].get<bool>()) return {}; Key result; result.fill(85); return result; }
std::string category(const fs::path &path) {
  const auto name = utf8(path.filename());
  if (name == "refs.json") return "refs";
  if (name == "config.json") return "config";
  const auto relative = path.lexically_relative(fixture_root / "primary/.syrnck/objects");
  if (!relative.empty() && *relative.begin() != "..") return utf8(*relative.begin());
  return "filesystem";
}
std::map<std::string, std::string> files(const fs::path &directory) {
  std::map<std::string, std::string> result;
  for (const auto &entry : fs::recursive_directory_iterator(directory))
    if (entry.is_regular_file()) result.emplace(utf8(entry.path().lexically_relative(directory)), text(read_bytes(entry.path())));
  return result;
}
void prepare(const fs::path &root, bool encrypted, bool partial, bool detached, bool case_only, bool abort) {
  Fixture f(root / "primary", key(Json{{"encrypted", encrypted}}));
  const auto head = f.revision(Json{{"Source/code", f.file("base")}, {"Content/asset", f.file("hidden history")}});
  f.install(head); f.store.create_native_branch("feature", head);
  atomic_write(root / "fixture.json", bytes(Json{{"encrypted", encrypted}, {"partial", partial}, {"detached", detached},
      {"case_only", case_only}, {"abort", abort}, {"head", head}}.dump()), false);
}
void bind(const fs::path &root, Json info) {
  Store store(root / "primary/.syrnck", key(info));
  const auto source = root / "edit", destination = root / (info["case_only"].get<bool>() ? "EDIT" : "moved");
  if (info["partial"].get<bool>()) store.register_partial_native_workspace(root / "primary", "edit", source, {"Source"});
  else store.register_native_workspace(root / "primary", "edit", source);
  store.use_native_workspace("edit");
  if (info["detached"].get<bool>()) store.detach_checkout(source, info["head"].get<std::string>());
  else store.switch_directory(source, "feature");
  atomic_write(source / "Source/code", bytes("prepared"), true);
  info["index"] = store.stage_directory(source, store.native_target(), {"Source"});
  info["index_ref"] = native_staging_reference(store, store.native_target());
  atomic_write(source / "Source/code", bytes("dirty"), true);
  atomic_write(source / "untracked.tmp", bytes("local ignored or excluded bytes"), false);
  fs::create_directories(source / ".git"); atomic_write(source / ".git/opaque", bytes("opaque local data"), false);
  fs::create_directory(source / "empty");
  info["files"] = files(source); info["identity"] = native_directory_identity(source)->json();
  info["source"] = utf8(fs::canonical(source)); info["destination"] = utf8(fs::canonical(destination.parent_path()) / destination.filename());
  store.use_native_workspace(""); store.pack();
  const auto plan = plan_native_workspace_relocation(store, "edit", destination); info["plan"] = plan["plan"];
  if (info["abort"].get<bool>()) {
    std::stop_source stop;
    fails([&] { relocate_native_workspace(store, "edit", destination, plan["plan"].get<std::string>(),
        {stop.get_token(), [&](const ProgressEvent &event) { if (event.path == "workspace-relocation/finalize") stop.request_stop(); }}); });
    require(native_workspace_relocation_status(store)["state"] == "destination", "abort fixture failed to pause after physical move");
  }
  atomic_write(root / "fixture.json", bytes(info.dump()), true);
}
void perform(Store &store, const fs::path &root, const Json &info) {
  if (info["abort"].get<bool>()) (void)recover_native_workspace_relocation(store, true);
  else (void)relocate_native_workspace(store, "edit", root / (info["case_only"].get<bool>() ? "EDIT" : "moved"), info["plan"].get<std::string>());
}
Json verify(Store &store, const fs::path &root, const Json &info, bool resumed) {
  store.fsck();
  const auto status = native_workspace_relocation_status(store);
  const auto refs = store.refs();
  require(refs.at("heads/main").id == info["head"].get<std::string>() && refs.at("heads/feature").id == info["head"].get<std::string>() &&
      refs.at(info["index_ref"].get<std::string>()).id == info["index"].get<std::string>(), "relocation changed history or frozen index");
  std::optional<fs::path> location;
  for (const auto &entry : fs::directory_iterator(root)) {
    const auto identity = native_directory_identity(entry.path());
    if (!identity || identity->json() != info["identity"]) continue;
    require(!location, "relocation duplicated its directory identity"); location = entry.path();
  }
  require(location && files(*location) == info["files"].get<std::map<std::string, std::string>>() &&
      fs::is_directory(*location / "empty"), "relocation lost working data or empty directories");
  const auto binding = parse_json(store.get(refs.at("workspaces/edit/registration")));
  const auto selection = parse_json(store.get(refs.at("workspaces/edit/workspace/HEAD")));
  require(binding["directory"] == selection["directory"] && selection["branch"].is_null() == info["detached"].get<bool>(),
      "relocation split HEAD and registration");
  if (status["pending"].get<bool>()) require(binding["directory"] == info["source"], "pending relocation changed logical binding");
  else require(binding["directory"] == utf8(fs::canonical(*location)), "completed relocation binding differs from actual directory");
  if (resumed) {
    require(status["pending"] == false && binding["directory"] == (info["abort"].get<bool>() ? info["source"] : info["destination"]),
        "relocation recovery did not reach requested state");
    store.use_native_workspace("edit");
    require(store.native_head()["revision"] == info["head"] && store.native_materialization()["prefixes"] ==
        (info["partial"].get<bool>() ? Json::array({"Source"}) : Json{}), "relocation changed logical HEAD or policy");
    (void)store.checkout_status(); store.gc(true); store.fsck();
    store.export_revision(info["head"].get<std::string>(), root / "complete");
    require(text(read_bytes(root / "complete/Content/asset")) == "hidden history", "relocation GC lost excluded objects");
  }
  return Json{{"verified", true}, {"pending", status["pending"]}};
}
}
namespace syrnck::testing {
void io_checkpoint(std::string_view event, const fs::path &path) {
  const auto type = category(path);
  if (tracing) { events.push_back(Json{{"phase", event}, {"kind", type}, {"occurrence", ++counts[{std::string(event), type}]}}); return; }
  if (!armed || event != checkpoint || type != kind || ++seen != occurrence) return;
  armed = false; std::cout << Json{{"phase", checkpoint}, {"kind", kind}, {"occurrence", seen}}.dump() << std::endl;
  std::string line; require(static_cast<bool>(std::getline(std::cin, line)) && line == "continue", "relocation checkpoint closed");
}
}
int main(int argc, char **argv) {
  try {
    require(argc >= 3, "relocation probe expects mode and root");
    const auto root = path_from_utf8(argv[2]); fixture_root = root;
    const std::string mode = argv[1];
    if (mode == "prepare") {
      require(argc == 8, "relocation probe expects encryption, selection, target, spelling and action");
      prepare(root, std::string_view(argv[3]) == "encrypted", std::string_view(argv[4]) == "partial",
          std::string_view(argv[5]) == "detached", std::string_view(argv[6]) == "case", std::string_view(argv[7]) == "abort");
    } else {
      const auto info = parse_json(read_bytes(root / "fixture.json"));
      if (mode == "bind") bind(root, info);
      else {
        Store store(root / "primary/.syrnck", key(info));
        if (mode == "trace" || mode == "run") {
          if (mode == "trace") tracing = true;
          else { require(argc == 6, "relocation probe expects checkpoint kind occurrence");
            checkpoint = argv[3]; kind = argv[4]; occurrence = static_cast<std::size_t>(std::stoull(argv[5])); armed = true; }
          perform(store, root, info);
          if (tracing) std::cout << events.dump() << '\n'; else require(!armed, "relocation checkpoint was not reached");
        } else {
          require(mode == "verify" || mode == "resume", "unknown relocation probe mode");
          if (mode == "resume") perform(store, root, info);
          std::cout << verify(store, root, info, mode == "resume").dump() << '\n';
        }
      }
    }
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
