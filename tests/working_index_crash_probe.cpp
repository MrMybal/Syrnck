#include <syrnck/working_index.hpp>
#include "native_workspace.hpp"
#include <iostream>
#include <sstream>
using namespace syrnck;
namespace {
bool armed = false;
std::string wanted, part;
std::optional<Key> key(bool encrypted) {
  if (!encrypted) return {};
  Key value{};
  value.fill(71);
  return value;
}
void prepare(const fs::path &root, bool encrypted, bool previous) {
  const auto project = root / "project";
  Store store(project / ".syrnck", key(encrypted), true);
  atomic_write(project / "asset", bytes("before"), false);
  const auto baseline = store.commit_directory(project, "main", "baseline");
  if (previous) indexed_native_status(store, project, baseline);
  atomic_write(project / "asset", bytes("after!"), true);
  atomic_write(root / "fixture.json", bytes(Json{{"encrypted", encrypted}, {"baseline", baseline},
                                                {"refs", encode_refs(store.refs())}}.dump()), false);
}
Json verify(const fs::path &root, bool resume) {
  const auto info = parse_json(read_bytes(root / "fixture.json"));
  const auto project = root / "project";
  Store store(project / ".syrnck", key(info["encrypted"].get<bool>()));
  require(encode_refs(store.refs()) == info["refs"], "index crash changed retained roots");
  require(text(read_bytes(project / "asset")) == "after!", "index crash changed working contents");
  const auto baseline = info["baseline"].get<std::string>();
  const auto value = store.metadata(Kind::revision, baseline);
  std::ostringstream original;
  store.reconstruct(value["files"]["asset"]["manifest"].get<std::string>(), original);
  require(original.str() == "before", "index crash lost retained baseline bytes");
  const auto full = store.status_directory(project, baseline);
  require(!full["clean"].get<bool>() && full["counts"]["modified"] == 1 && full["hashed_bytes"] == 6,
          "full status after index crash lost working edit");
  const auto pointer_path = store.root() / "working-index.json";
  if (fs::exists(pointer_path)) {
    const auto pointer = parse_json(read_bytes(pointer_path));
    const auto cache = parse_json(store.get({Kind::chunk, pointer["object"].get<std::string>()}));
    require(cache["type"] == "native-working-index", "index crash published invalid pointer/object");
  }
  if (resume) {
    const auto indexed = indexed_native_status(store, project, baseline);
    require(indexed["changes"] == full["changes"] && indexed["counts"] == full["counts"],
            "resumed indexed status trusted stale data after crash");
  }
  return Json{{"verified", true}};
}
} // namespace
namespace syrnck::testing {
void io_checkpoint(std::string_view phase, const fs::path &target) {
  const auto kind = target.filename() == "working-index.json" ? "pointer"
                    : target.parent_path().parent_path().filename() == "chunk" ? "object" : "other";
  if (!armed || phase != wanted || part != kind) return;
  armed = false;
  std::cout << Json{{"phase", phase}, {"part", part}}.dump() << std::endl;
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
      require(argc == 5, "probe expects encryption and previous index flags");
      prepare(root, std::string_view(argv[3]) == "encrypted", std::string_view(argv[4]) == "previous");
    } else if (mode == "run") {
      require(argc == 5, "probe expects phase and part");
      wanted = argv[3];
      part = argv[4];
      const auto info = parse_json(read_bytes(root / "fixture.json"));
      const auto project = root / "project";
      Store store(project / ".syrnck", key(info["encrypted"].get<bool>()));
      armed = true;
      indexed_native_status(store, project, info["baseline"].get<std::string>());
      require(!armed, "index checkpoint was not reached");
    } else {
      require(mode == "verify" || mode == "resume", "unknown probe mode");
      std::cout << verify(root, mode == "resume").dump() << '\n';
    }
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
