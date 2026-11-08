#include "native_history_fixture.hpp"
#include <syrnck.h>
#include <iostream>
using namespace native_test;
namespace {
bool trace = false, armed = false;
std::string wanted;
std::size_t occurrence = 0, seen = 0;
Json events = Json::array();
std::map<std::string, std::size_t> counts;
std::optional<Key> encryption(const Json &fixture) {
  if (!fixture["encrypted"].get<bool>()) return {};
  Key key; key.fill(70); return key;
}
void prepare(const fs::path &root, bool encrypted, unsigned layout) {
  Fixture f(root, encryption(Json{{"encrypted", encrypted}}));
  const auto old = f.revision(Json{{"data/asset", f.file("old asset")}, {"catalog.json", f.file("old protocol counters")}});
  const auto next = f.revision(Json{{"data/asset", f.file("new asset")}, {"catalog.json", f.file("new protocol counters")}}, Json::array({old}));
  f.store.set_ref("ffi/catalog/tip", {Kind::revision, old});
  f.store.set_ref("ffi/catalog/checkpoint", {Kind::revision, old});
  f.pack(layout);
  atomic_write(root / "fixture.json", bytes(Json{{"encrypted", encrypted}, {"old", old}, {"next", next}}.dump()), false);
}
void publish(const fs::path &root, const Json &fixture) {
  const auto key = encryption(fixture);
  SyrnckStore *store = nullptr;
  require(syrnck_store_open(utf8(root / ".syrnck").c_str(), false, key ? key->data() : nullptr, key ? key->size() : 0, &store) == 0,
      syrnck_last_error());
  struct Close { SyrnckStore *store; ~Close() { syrnck_store_close(store); } } close{store};
  const Json old{{"kind", "revision"}, {"id", fixture["old"]}}, next{{"kind", "revision"}, {"id", fixture["next"]}};
  const auto document = Json::array({Json{{"name", "ffi/catalog/tip"}, {"expected", old}, {"value", next}},
      Json{{"name", "ffi/catalog/checkpoint"}, {"expected", old}, {"value", next}},
      Json{{"name", "archive/catalog/previous"}, {"expected", nullptr}, {"value", old}}}).dump();
  int applied = 0;
  const auto code = syrnck_refs_compare_json(store, bytes(document).data(), document.size(), nullptr, &applied);
  require(code == 0 && applied == 1, syrnck_last_error());
}
void verify(const fs::path &root, const Json &fixture, bool finish) {
  bool old;
  {
    Store store(root / ".syrnck", encryption(fixture)); store.fsck();
    const auto refs = store.refs();
    old = refs.at("ffi/catalog/tip").id == fixture["old"].get<std::string>();
    const auto target = (old ? fixture["old"] : fixture["next"]).get<std::string>();
    require(refs.at("ffi/catalog/tip").id == target && refs.at("ffi/catalog/checkpoint").id == target &&
        refs.contains("archive/catalog/previous") != old, "catalog CAS exposed a mixed transaction");
    if (!old) require(refs.at("archive/catalog/previous").id == fixture["old"].get<std::string>(), "catalog archive lost old history");
    (void)store.closure({{Kind::revision, target}}, true);
  }
  if (finish) {
    if (old) publish(root, fixture); // Retry only the unchanged exact original condition.
    Store store(root / ".syrnck", encryption(fixture)); store.gc(true); store.fsck();
    require(store.refs().at("ffi/catalog/tip").id == fixture["next"].get<std::string>(), "catalog retry failed");
    store.export_revision(fixture["old"].get<std::string>(), root / "old");
    store.export_revision(fixture["next"].get<std::string>(), root / "next");
    require(text(read_bytes(root / "old/data/asset")) == "old asset" && text(read_bytes(root / "next/data/asset")) == "new asset" &&
        text(read_bytes(root / "old/catalog.json")) == "old protocol counters" &&
        text(read_bytes(root / "next/catalog.json")) == "new protocol counters", "catalog restart/GC lost data or causal state");
  }
}
}
namespace syrnck::testing {
void io_checkpoint(std::string_view phase, const fs::path &) {
  if (trace) { events.push_back(Json{{"phase", phase}, {"occurrence", ++counts[std::string(phase)]}}); return; }
  if (!armed || phase != wanted || ++seen != occurrence) return;
  armed = false;
  std::cout << Json{{"phase", phase}, {"occurrence", seen}}.dump() << std::endl;
  std::string line; require(static_cast<bool>(std::getline(std::cin, line)) && line == "continue", "catalog checkpoint closed");
}
}
int main(int argc, char **argv) {
  try {
    require(argc >= 3, "catalog probe expects mode and root");
    const std::string mode = argv[1]; const auto root = path_from_utf8(argv[2]);
    if (mode == "prepare") {
      require(argc == 5, "catalog probe expects encryption and layout");
      prepare(root, std::string_view(argv[3]) == "encrypted", static_cast<unsigned>(std::stoul(argv[4])));
    } else {
      const auto fixture = parse_json(read_bytes(root / "fixture.json"));
      if (mode == "trace" || mode == "run") {
        if (mode == "trace") trace = true;
        else { require(argc == 5, "catalog probe expects phase and occurrence"); wanted = argv[3]; occurrence = std::stoull(argv[4]); armed = true; }
        publish(root, fixture);
        if (trace) std::cout << events.dump() << '\n'; else require(!armed, "catalog checkpoint not observed");
      } else {
        require(mode == "verify" || mode == "resume", "unknown catalog probe mode");
        verify(root, fixture, mode == "resume"); std::cout << Json{{"verified", true}}.dump() << '\n';
      }
    }
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
