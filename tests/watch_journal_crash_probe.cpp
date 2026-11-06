#include "watch_journal_fixture.hpp"
#include <iostream>
#include <sstream>
using namespace syrnck;
namespace {
bool armed = false;
std::string wanted;
void prepare(const fs::path &root, bool encrypted, bool previous) {
  const auto project = root / "project";
  Store store(project / ".syrnck", watch_fixture::key(encrypted), true);
  atomic_write(project / "asset", bytes("retained"), false);
  const auto baseline = store.commit_directory(project, "main", "baseline");
  if (previous) store.write_workspace_watch_journal(watch_fixture::record());
  atomic_write(project / "asset", bytes("working!"), true);
  atomic_write(root / "fixture.json", bytes(Json{{"encrypted", encrypted}, {"previous", previous},
      {"baseline", baseline}, {"refs", encode_refs(store.refs())}}.dump()), false);
}
Json verify(const fs::path &root, bool resume) {
  const auto info = parse_json(read_bytes(root / "fixture.json"));
  const auto project = root / "project";
  Store store(project / ".syrnck", watch_fixture::key(info["encrypted"].get<bool>()));
  require(encode_refs(store.refs()) == info["refs"], "journal crash changed retained roots");
  require(text(read_bytes(project / "asset")) == "working!", "journal crash changed working bytes");
  const auto revision = store.metadata(Kind::revision, info["baseline"].get<std::string>());
  std::ostringstream original;
  store.reconstruct(revision["files"]["asset"]["manifest"].get<std::string>(), original);
  require(original.str() == "retained", "journal crash lost retained asset");
  const auto record = store.workspace_watch_journal();
  require((record.is_null() && !info["previous"].get<bool>()) || record == watch_fixture::record() ||
              record == watch_fixture::record(true), "journal crash left a partial or invalid record");
  if (resume) {
    store.write_workspace_watch_journal(watch_fixture::record(true));
    require(store.workspace_watch_journal() == watch_fixture::record(true), "journal resume failed");
  }
  return Json{{"verified", true}};
}
} // namespace
namespace syrnck::testing {
void io_checkpoint(std::string_view phase, const fs::path &target) {
  if (!armed || target.filename() != "watch-journal" || phase != wanted) return;
  armed = false;
  std::cout << Json{{"phase", phase}}.dump() << std::endl;
  std::string line;
  require(static_cast<bool>(std::getline(std::cin, line)) && line == "continue", "checkpoint closed");
}
} // namespace syrnck::testing
int main(int argc, char **argv) {
  try {
    require(argc >= 3, "probe expects mode and root");
    const std::string mode = argv[1];
    const auto root = path_from_utf8(argv[2]);
    if (mode == "prepare") {
      require(argc == 5, "probe expects encryption and previous journal flags");
      prepare(root, std::string_view(argv[3]) == "encrypted", std::string_view(argv[4]) == "previous");
    } else if (mode == "run") {
      require(argc == 4, "probe expects phase");
      wanted = argv[3];
      const auto info = parse_json(read_bytes(root / "fixture.json"));
      Store store(root / "project/.syrnck", watch_fixture::key(info["encrypted"].get<bool>()));
      armed = true;
      store.write_workspace_watch_journal(watch_fixture::record(true));
      require(!armed, "journal checkpoint was not reached");
    } else {
      require(mode == "verify" || mode == "resume", "unknown probe mode");
      std::cout << verify(root, mode == "resume").dump() << '\n';
    }
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
