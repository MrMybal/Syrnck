#include "structured_archive_fixture.hpp"
#include <iostream>
using namespace structured_archive_test;
namespace {
bool armed = false;
std::string wanted;
std::optional<Key> source_key(const Json &info) {
  return encryption_key(info["encrypted"].get<bool>(), 47);
}
std::optional<Key> backup_key(const Json &info) {
  return encryption_key(info["encrypted"].get<bool>(), 59);
}
void prepare(const fs::path &root, bool encrypted) {
  Json info{{"encrypted", encrypted}};
  Fixture f(root / "project", source_key(info));
  const auto a = populate(f);
  f.store.pack();
  create_backup(f.store, root / "backup", backup_key(info));
  reuse_names(f.store, a);
  const auto refs = f.store.refs();
  info["archives"] = a.json();
  info["before"] = encode_refs(refs);
  info["after"] = encode_refs(retired_refs(refs, a.selectors));
  atomic_write(root / "fixture.json", bytes(info.dump()), false);
}
void perform(const fs::path &root, const Json &info) {
  Store store(root / "project/.syrnck", source_key(info));
  if (encode_refs(store.refs()) == info["after"])
    return;
  Store backup(root / "backup", backup_key(info));
  const auto selectors = info["archives"]["selectors"].get<std::vector<std::string>>();
  const auto plan = plan_archive_retirement(store, backup, selectors);
  retire_archives(store, backup, selectors, plan["plan"].get<std::string>());
}
Json verify(const fs::path &root, const Json &info, bool resumed) {
  Store store(root / "project/.syrnck", source_key(info));
  if (resumed)
    store.gc(true);
  store.fsck();
  const auto refs = encode_refs(store.refs());
  require(refs == info["before"] || refs == info["after"], "retirement crash split archive groups");
  require(!resumed || refs == info["after"], "structured retirement did not resume");
  require(text(read_bytes(root / "project/asset")) == "dirty current" &&
              text(read_bytes(root / "project/untracked")) == "leave alone",
          "retirement touched workspace");
  Store backup(root / "backup", backup_key(info));
  verify_backup(backup);
  const auto &a = info["archives"];
  check_bytes(backup, a["tip"].get<std::string>(), "old branch tip");
  check_bytes(backup, a["parent"].get<std::string>(), "original");
  check_bytes(backup, a["index"].get<std::string>(), "prepared archived index");
  require(backup.native_tag_archive(a["tag"].get<std::string>(), true)["tag"]["message"] ==
              "Retained notes: \xc3\xa9",
          "retirement lost covered annotation");
  return Json{{"verified", true}, {"retired", refs == info["after"]}};
}
} // namespace
namespace syrnck::testing {
void io_checkpoint(std::string_view phase, const fs::path &target) {
  if (!armed || phase != wanted || target.filename() != "refs.json")
    return;
  armed = false;
  std::cout << Json{{"phase", phase}}.dump() << std::endl;
  std::string line;
  require(static_cast<bool>(std::getline(std::cin, line)) && line == "continue",
          "retirement checkpoint closed");
}
} // namespace syrnck::testing
int main(int argc, char **argv) {
  try {
    require(argc >= 3, "structured retirement probe expects mode and root");
    const std::string mode = argv[1];
    const auto root = path_from_utf8(argv[2]);
    if (mode == "prepare") {
      require(argc == 4, "structured retirement probe expects encryption option");
      prepare(root, std::string_view(argv[3]) == "encrypted");
    } else {
      const auto info = parse_json(read_bytes(root / "fixture.json"));
      if (mode == "run") {
        require(argc == 4, "structured retirement probe expects checkpoint");
        wanted = argv[3];
        armed = true;
        perform(root, info);
        require(!armed, "retirement checkpoint was not reached");
      } else {
        require(mode == "verify" || mode == "resume", "unknown structured retirement probe mode");
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
