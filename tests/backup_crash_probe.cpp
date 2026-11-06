#include "native_history_fixture.hpp"
#include <syrnck/backup.hpp>
#include <syrnck/bisect.hpp>
#include <iostream>
using namespace native_test;
namespace {
std::string wanted_phase, wanted_kind;
std::size_t wanted_count = 0, seen = 0;
bool armed = false;
std::optional<Key> key(bool encrypted, std::uint8_t fill) {
  if (!encrypted)
    return {};
  Key result;
  result.fill(fill);
  return result;
}
std::string kind(const fs::path &target) {
  const auto name = utf8(target.filename());
  if (name == "backup" || name == "restored")
    return name;
  if (name == "backup.json")
    return "descriptor";
  if (name == "refs.json")
    return "refs";
  if (name == "config.json")
    return "config";
  if (name == "settings.json")
    return "settings";
  return utf8(target.parent_path().parent_path().filename());
}
void prepare(const fs::path &root, bool encrypted, const std::string &operation) {
  Fixture f(root / "source", key(encrypted, 57));
  const auto old_file = f.file("original retained asset"), new_file = f.file("updated retained asset");
  const auto first = f.revision(Json{{"asset.bin", old_file}});
  const bool bisect = operation.starts_with("bisect-");
  const auto middle = bisect ? f.revision(Json{{"asset.bin", old_file}}, Json::array({first})) : first;
  const auto last = f.revision(Json{{"asset.bin", new_file}}, Json::array({middle}));
  f.install(last);
  Json state;
  if (bisect && operation != "bisect-start")
    state = NativeBisect(f.store).start(first, last);
  const auto refs = f.store.refs();
  if (operation != "create" && !bisect)
    create_backup(f.store, root / "backup", key(encrypted, 65));
  if (operation == "repair") {
    const auto manifest = new_file["manifest"].get<std::string>();
    const auto metadata = f.store.metadata(Kind::manifest, manifest);
    const auto chunk = metadata["blocks"][0]["id"].get<std::string>();
    fs::remove(f.store.root() / "objects/chunk" / chunk.substr(0, 2) / chunk);
    fs::remove(f.store.root() / "objects/manifest" / manifest.substr(0, 2) / manifest);
  }
  const auto info = Json{{"encrypted", encrypted},
                         {"operation", operation},
                         {"first", first},
                         {"last", last},
                         {"middle", middle},
                         {"bisect_state", bisect && operation != "bisect-start" ? state["state"] : Json{}},
                         {"old_manifest", old_file["manifest"]},
                         {"new_manifest", new_file["manifest"]},
                         {"refs", encode_refs(refs)}};
  atomic_write(root / "fixture.json", bytes(info.dump()), false);
}
void perform(const fs::path &root, const Json &info) {
  const auto encrypted = info["encrypted"].get<bool>();
  const auto operation = info["operation"].get<std::string>();
  if (operation.starts_with("bisect-")) {
    Store source(root / "source/.syrnck", key(encrypted, 57));
    NativeBisect search(source);
    const auto state = search.status();
    if (operation == "bisect-start") {
      if (!state["active"].get<bool>())
        search.start(info["first"].get<std::string>(), info["last"].get<std::string>());
    } else if (operation == "bisect-mark") {
      if (state["state"] == info["bisect_state"])
        search.mark("good", info["middle"].get<std::string>(), info["bisect_state"].get<std::string>());
    } else if (state["active"].get<bool>())
      search.reset(info["bisect_state"].get<std::string>());
  } else if (operation == "create") {
    Store source(root / "source/.syrnck", key(encrypted, 57));
    if (!fs::exists(root / "backup"))
      create_backup(source, root / "backup", key(encrypted, 65));
  } else {
    Store backup(root / "backup", key(encrypted, 65));
    if (operation == "restore") {
      if (!fs::exists(root / "restored"))
        restore_backup(backup, root / "restored", key(encrypted, 67));
    } else {
      Store destination(root / "source/.syrnck", key(encrypted, 57));
      repair_from_backup(destination, backup, true);
    }
  }
}
Json verify(const fs::path &root, const Json &info, bool resumed) {
  const auto encrypted = info["encrypted"].get<bool>();
  const auto operation = info["operation"].get<std::string>();
  Store source(root / "source/.syrnck", key(encrypted, 57));
  if (operation.starts_with("bisect-")) {
    const auto refs = source.refs();
    require(refs.at("heads/main") == parse_key(info["refs"]["heads/main"]), "bisect crash moved its branch");
    const auto state = NativeBisect(source).status();
    if (state["active"].get<bool>()) {
      require(state["initial_good"] == info["first"] && state["initial_bad"] == info["last"],
              "bisect crash changed its search roots");
      require(refs.size() == 3, "bisect crash left unexpected protection references");
      if (resumed && operation == "bisect-mark")
        require(state["status"] == "found" && state["first_bad"] == info["last"],
                "bisect mark did not resume");
    } else {
      const bool archived = refs.contains("archive/bisect/" + info["last"].get<std::string>());
      require(operation == "bisect-reset" ? archived && refs.size() == 3
                                          : operation == "bisect-start" && refs.size() == 1,
              "bisect crash split its state and protection roots");
    }
    if (resumed)
      require(state["active"].get<bool>() == (operation != "bisect-reset"),
              "bisect crash did not resume its session state");
    source.fsck();
    for (const auto &field : {"old_manifest", "new_manifest"}) {
      std::ostringstream bytes;
      source.reconstruct(info[field].get<std::string>(), bytes);
      require(bytes.str() == (std::string_view(field) == "old_manifest" ? "original retained asset"
                                                                        : "updated retained asset"),
              "bisect crash lost retained bytes");
    }
    return Json{{"verified", true}, {"published", state["active"]}};
  }
  require(encode_refs(source.refs()) == info["refs"], "backup crash changed source references");
  if (operation == "repair") {
    Store backup(root / "backup", key(encrypted, 65));
    const auto plan = repair_from_backup(source, backup);
    require(plan["ready"] == true && (!resumed || plan["missing"].empty()),
            "missing repair objects after resume");
  } else
    source.fsck();
  const auto target = root / (operation == "restore" ? "restored" : "backup");
  const auto present = fs::exists(target);
  require(!resumed || present, "resumed backup destination is absent");
  if (present) {
    Store result(target, key(encrypted, operation == "restore" ? 67 : 65));
    if (operation != "restore")
      verify_backup(result);
    result.fsck();
    require(encode_refs(result.refs()) == info["refs"], "backup crash lost retained references");
    for (const auto &name : {"old_manifest", "new_manifest"}) {
      std::ostringstream bytes;
      result.reconstruct(info[name].get<std::string>(), bytes);
      require(bytes.str() == (std::string_view(name) == "old_manifest" ? "original retained asset"
                                                                       : "updated retained asset"),
              "backup crash lost retained bytes");
    }
  }
  return Json{{"verified", true}, {"published", present}};
}
} // namespace
namespace syrnck::testing {
void io_checkpoint(std::string_view phase, const fs::path &target) {
  if (!armed || phase != wanted_phase || kind(target) != wanted_kind || ++seen != wanted_count)
    return;
  armed = false;
  std::cout << Json{{"phase", phase}, {"kind", wanted_kind}, {"target", utf8(target)}}.dump() << std::endl;
  std::string line;
  require(static_cast<bool>(std::getline(std::cin, line)) && line == "continue", "backup checkpoint closed");
}
} // namespace syrnck::testing
int main(int argc, char **argv) {
  try {
    require(argc >= 3, "backup crash probe expects mode and root");
    const auto root = path_from_utf8(argv[2]);
    const std::string mode = argv[1];
    if (mode == "prepare") {
      require(argc == 5, "backup crash probe expects encryption and operation");
      prepare(root, std::string_view(argv[3]) == "encrypted", argv[4]);
    } else {
      const auto info = parse_json(read_bytes(root / "fixture.json"));
      if (mode == "run") {
        require(argc == 6, "backup crash probe expects phase kind and occurrence");
        wanted_phase = argv[3];
        wanted_kind = argv[4];
        wanted_count = static_cast<std::size_t>(std::stoull(argv[5]));
        armed = true;
        perform(root, info);
        require(!armed, "backup checkpoint was not reached");
      } else {
        require(mode == "verify" || mode == "resume", "unknown backup crash probe mode");
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
