#include "native_history_fixture.hpp"
#include "native_target.hpp"
#include <syrnck/workspace.hpp>
#include <iostream>
using namespace native_test;
namespace {
bool armed = false, tracing = false;
std::string phase, kind;
std::size_t occurrence = 0, seen = 0;
Json events = Json::array();
std::map<std::pair<std::string, std::string>, std::size_t> counts;
fs::path fixture_root;
std::optional<Key> key(const Json &info) {
  if (!info["encrypted"].get<bool>()) return {};
  Key result; result.fill(82); return result;
}
std::string category(const fs::path &path) {
  const auto name = utf8(path.filename());
  if (name == "refs.json") return "refs";
  if (name == "config.json") return "config";
  const auto relative = path.lexically_relative(fixture_root / "primary/.syrnck/objects");
  if (!relative.empty() && *relative.begin() != "..") return utf8(*relative.begin());
  return "filesystem";
}
std::map<std::string, std::string> working(const fs::path &directory) {
  std::map<std::string, std::string> result;
  for (const auto &entry : fs::recursive_directory_iterator(directory))
    if (entry.is_regular_file()) result.emplace(utf8(entry.path().lexically_relative(directory)), text(read_bytes(entry.path())));
  return result;
}
void prepare(const fs::path &root, bool encrypted, bool partial, bool detached, std::string action) {
  Fixture f(root / "primary", key(Json{{"encrypted", encrypted}}));
  const auto head = f.revision(Json{{"Source/code", f.file("base")}, {"Content/A/leaf", f.file("leaf")},
      {"Content/B", f.file("hidden")}});
  f.install(head); f.store.create_native_branch("feature", head);
  atomic_write(root / "fixture.json", bytes(Json{{"encrypted", encrypted}, {"partial", partial},
      {"detached", detached}, {"action", action}, {"head", head}}.dump()), false);
}
void bind(const fs::path &root, Json info) {
  Store store(root / "primary/.syrnck", key(info));
  const auto edit = root / "edit";
  if (info["partial"].get<bool>()) store.register_partial_native_workspace(root / "primary", "edit", edit, {"Source", "Content/A/leaf"});
  else store.register_native_workspace(root / "primary", "edit", edit);
  store.use_native_workspace("edit");
  if (info["detached"].get<bool>()) store.detach_checkout(edit, info["head"].get<std::string>());
  else store.switch_directory(edit, "feature");
  const auto write = [&](std::string_view name, std::string_view value) {
    const auto file = edit / path_from_utf8(name); fs::create_directories(file.parent_path());
    atomic_write(file, bytes(value), true);
  };
  write("Source/code", "staged"); write("Source/new", "staged new");
  info["index"] = store.stage_directory(edit, store.native_target(), {"Source"});
  write("Source/code", "dirty"); write("Source/new", "dirty new");
  write("Source/untracked", "selected untracked"); write("outside", "excluded work");
  fs::remove_all(edit / "Content"); write("Content", "physical ancestor");
  info["before"] = working(edit); info["captured"] = info["before"];
  if (info["partial"].get<bool>()) info["captured"].erase("outside");
  info["archive"] = Json{};
  if (info["action"] == "restore") {
    const auto plan = plan_native_workspace_unregistration(store, true);
    info["archive"] = unregister_native_workspace(store, plan["plan"].get<std::string>(), true)["archive"];
  }
  store.pack();
  atomic_write(root / "fixture.json", bytes(info.dump()), true);
}
void perform(Store &store, const fs::path &root, const Json &info) {
  if (info["action"] == "unregister") {
    if (store.refs().contains("workspaces/edit/registration")) {
      store.use_native_workspace("edit");
      const auto plan = plan_native_workspace_unregistration(store, true);
      unregister_native_workspace(store, plan["plan"].get<std::string>(), true);
    }
  } else {
    NativeWorkspaceRestoreOptions settings; settings.detached = true;
    const auto id = info["archive"].get<std::string>();
    const auto plan = plan_native_workspace_archive_restore(store, root / "primary", id, "restored", root / "restored", settings);
    restore_native_workspace_archive(store, root / "primary", id, "restored", root / "restored",
        plan["plan"].get<std::string>(), settings);
  }
}
Json verify(Store &store, const fs::path &root, const Json &info, bool resumed) {
  if (resumed) store.gc(true);
  store.fsck();
  const auto refs = store.refs();
  const bool removed = !refs.contains("workspaces/edit/registration");
  const auto inventory = native_workspace_archives(store);
  require(inventory["archives"].size() == (removed ? 1U : 0U), "workspace archive and registration publication split");
  require(working(root / "edit") == info["before"].get<std::map<std::string, std::string>>(), "unregistration changed source files");
  require(refs.at("heads/main").id == info["head"].get<std::string>() && refs.at("heads/feature").id == info["head"].get<std::string>(),
      "workspace archive operation changed shared branches");
  if (removed) {
    for (const auto &[name, object] : refs) { (void)object; require(!name.starts_with("workspaces/edit/"), "unregistration left partial context roots"); }
    const auto archive = native_workspace_archive(store, inventory["archives"][0]["id"].get<std::string>(), true);
    require(archive["revision"] == info["head"] && archive["index"] == info["index"], "workspace archive lost head/index identity");
    const auto snapshot = store.metadata(Kind::revision, archive["work"].get<std::string>());
    require(snapshot["files"].size() == info["captured"].size(), "workspace archive lost physical paths");
    for (const auto &[name, file] : snapshot["files"].items()) {
      std::ostringstream output; store.reconstruct(file["manifest"].get<std::string>(), output);
      require(info["captured"].contains(name) && output.str() == info["captured"][name].get<std::string>(), "workspace archive lost physical bytes");
    }
  }
  const bool registered = refs.contains("workspaces/restored/registration");
  if (info["action"] == "restore") {
    if (fs::exists(root / "restored"))
      require(working(root / "restored") == info["captured"].get<std::map<std::string, std::string>>(), "workspace restore exposed partial files");
    if (registered) {
      require(fs::is_directory(root / "restored"), "workspace restored bindings before files");
      store.use_native_workspace("restored");
      require(store.native_head()["revision"] == info["head"] && store.native_head()["branch"].is_null(), "workspace restore published wrong HEAD");
      const auto staged = refs.at(native_staging_reference(store, detached_native_target));
      require(store.metadata(Kind::revision, staged.id)["files"] == store.metadata(Kind::revision, info["index"].get<std::string>())["files"],
          "workspace restore split dirty work and frozen index");
      require(store.native_materialization()["prefixes"] == (info["partial"].get<bool>() ? Json::array({"Content/A/leaf", "Source"}) : Json{}),
          "workspace restore lost its partial selection");
      store.use_native_workspace("");
    }
  }
  require(!resumed || (removed && (info["action"] == "unregister" || registered)), "workspace archive operation did not finish on retry");
  return Json{{"verified", true}, {"unregistered", removed}, {"restored", registered}};
}
}
namespace syrnck::testing {
void io_checkpoint(std::string_view event, const fs::path &path) {
  const auto type = category(path);
  if (tracing) { events.push_back(Json{{"phase", event}, {"kind", type}, {"occurrence", ++counts[{std::string(event), type}]}}); return; }
  if (!armed || event != phase || type != kind || ++seen != occurrence) return;
  armed = false;
  std::cout << Json{{"phase", phase}, {"kind", kind}, {"occurrence", seen}}.dump() << std::endl;
  std::string line; require(static_cast<bool>(std::getline(std::cin, line)) && line == "continue", "workspace archive checkpoint closed");
}
}
int main(int argc, char **argv) {
  try {
    require(argc >= 3, "workspace archive probe expects mode and root");
    const auto root = path_from_utf8(argv[2]); fixture_root = root;
    const std::string mode = argv[1];
    if (mode == "prepare") {
      require(argc == 7, "workspace archive probe expects encryption, selection, target and action");
      prepare(root, std::string_view(argv[3]) == "encrypted", std::string_view(argv[4]) == "partial",
          std::string_view(argv[5]) == "detached", argv[6]);
    } else {
      const auto info = parse_json(read_bytes(root / "fixture.json"));
      if (mode == "bind") bind(root, info);
      else {
        Store store(root / "primary/.syrnck", key(info));
        if (mode == "trace" || mode == "run") {
          if (mode == "trace") tracing = true;
          else { require(argc == 6, "workspace archive probe expects phase kind occurrence");
            phase = argv[3]; kind = argv[4]; occurrence = static_cast<std::size_t>(std::stoull(argv[5])); armed = true; }
          perform(store, root, info);
          if (tracing) std::cout << events.dump() << '\n';
          else require(!armed, "workspace archive checkpoint was not reached");
        } else {
          require(mode == "verify" || mode == "resume", "unknown workspace archive probe mode");
          if (mode == "resume") perform(store, root, info);
          std::cout << verify(store, root, info, mode == "resume").dump() << '\n';
        }
      }
    }
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
