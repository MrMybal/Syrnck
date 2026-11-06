#include "native_history_fixture.hpp"
#include <syrnck/backup.hpp>
#include <syrnck/checkout_retention.hpp>
#include <syrnck/workspace.hpp>
#include <iostream>
using namespace native_test;
namespace {
bool armed = false;
std::string wanted;
std::optional<Key> key(const Json &info) {
  if (!info["encrypted"].get<bool>())
    return {};
  Key result;
  result.fill(79);
  return result;
}
void prepare(const fs::path &root, bool encrypted, bool sources, bool retained) {
  Fixture f(root / "project", key(Json{{"encrypted", encrypted}}));
  const auto old = f.revision(Json{{"asset", f.file("original")}});
  const auto target = f.revision(Json{{"asset", f.file("independent new head")}});
  f.install(old);
  f.write("asset", "dirty retained source");
  const auto planned = plan_native_hard_reset(f.store, f.project, "main", target);
  const auto reset =
      native_hard_reset(f.store, f.project, "main", old, target, planned["plan"].get<std::string>());
  f.store.pack();
  create_backup(f.store, root / "backup", key(Json{{"encrypted", encrypted}}));
  Store backup(root / "backup", key(Json{{"encrypted", encrypted}}));
  const auto id = reset["id"].get<std::string>();
  std::string directory = "project/.syrnck";
  auto refs = f.store.refs();
  if (retained) {
    restore_backup(backup, root / "restored", key(Json{{"encrypted", encrypted}}));
    directory = "restored";
    Store restored(root / "restored", key(Json{{"encrypted", encrypted}}));
    refs = restored.refs();
  }
  auto after = refs;
  after.erase("checkouts/receipts/" + id);
  after.erase("checkouts/completed/" + id);
  if (sources)
    after.erase("archive/checkout/" + id);
  atomic_write(root / "fixture.json",
               bytes(Json{{"encrypted", encrypted},
                          {"sources", sources},
                          {"directory", directory},
                          {"id", id},
                          {"source", reset["source"]},
                          {"before", encode_refs(refs)},
                          {"after", encode_refs(after)}}
                         .dump()),
               false);
}
void perform(const fs::path &root, const Json &info) {
  Store store(root / path_from_utf8(info["directory"].get<std::string>()), key(info));
  if (!store.refs().contains("checkouts/completed/" + info["id"].get<std::string>()))
    return;
  Store backup(root / "backup", key(info));
  const std::vector<std::string> ids{info["id"].get<std::string>()};
  const bool sources = info["sources"].get<bool>();
  const auto plan = plan_checkout_retirement(store, backup, ids, sources);
  retire_checkouts(store, backup, ids, plan["plan"].get<std::string>(), sources);
}
Json verify(const fs::path &root, const Json &info, bool resumed) {
  Store store(root / path_from_utf8(info["directory"].get<std::string>()), key(info));
  if (resumed)
    store.gc(true);
  store.fsck();
  const auto refs = encode_refs(store.refs());
  require(refs == info["before"] || refs == info["after"], "checkout retirement crash split reference group");
  require(!resumed || refs == info["after"], "checkout retirement did not resume");
  require(text(read_bytes(root / "project/asset")) == "independent new head",
          "checkout retirement touched workspace");
  Store backup(root / "backup", key(info));
  verify_backup(backup);
  const auto value = backup.metadata(Kind::revision, info["source"].get<std::string>());
  std::ostringstream output;
  backup.reconstruct(value["files"]["asset"]["manifest"].get<std::string>(), output);
  require(output.str() == "dirty retained source", "checkout retirement lost covered bytes");
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
    require(argc >= 3, "retirement probe expects mode and root");
    const std::string mode = argv[1];
    const auto root = path_from_utf8(argv[2]);
    if (mode == "prepare") {
      require(argc == 6, "retirement probe expects encryption, source and receipt options");
      prepare(root, std::string_view(argv[3]) == "encrypted", std::string_view(argv[4]) == "sources",
              std::string_view(argv[5]) == "retained");
    } else {
      const auto info = parse_json(read_bytes(root / "fixture.json"));
      if (mode == "run") {
        require(argc == 4, "retirement probe expects checkpoint");
        wanted = argv[3];
        armed = true;
        perform(root, info);
        require(!armed, "retirement checkpoint was not reached");
      } else {
        require(mode == "verify" || mode == "resume", "unknown retirement probe mode");
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
