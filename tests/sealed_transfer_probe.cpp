#include "internal.hpp"
#include <syrnck/sealed_net.hpp>
#include <syrnck/native_sync.hpp>
#include <cstdlib>
#include <iostream>
using namespace syrnck;
namespace {
fs::path plain_path;
std::string phase;
bool armed = false;
bool agent = false;
void wait_for_parent() {
  armed = false;
  std::cout << "{\"paused\":true}" << std::endl;
  std::string line;
  require(static_cast<bool>(std::getline(std::cin, line)) && line == "continue",
          "sealed probe checkpoint closed");
}
int entry(int argc, char **argv) {
  try {
    require(argc == 7, "sealed probe expects operation plaintext wire URL checkpoint encryption");
    std::optional<Key> at_rest;
    if (std::string_view(argv[6]) == "encrypted") {
      at_rest.emplace();
      at_rest->fill(0x39);
    }
    Store plain(path_from_utf8(argv[2]), at_rest);
    plain_path = plain.root();
    const auto location = path_from_utf8(argv[3]);
    Store wire(location, {}, !fs::is_regular_file(location / "config.json"));
    Key master;
    master.fill(0x71);
    const auto key = SecretKey::from_bytes(master);
    const auto *token = std::getenv("SYRNCK_TOKEN");
    require(token, "missing sealed probe token");
    const net::Remote remote(argv[4], token);
    phase = argv[5];
    agent = std::string_view(argv[1]) == "agent";
    armed = true;
    OperationOptions options{{}, [&](const ProgressEvent &event) {
                               if (armed && phase == "transfer" &&
                                   event.phase == ProgressPhase::transferring &&
                                   event.path.starts_with("v1/objects/") && event.bytes)
                                 wait_for_parent();
                             }};
    if (std::string_view(argv[1]) == "push") {
      const auto root = plain.refs().at("heads/main");
      (void)net::push_sealed_snapshot(plain, wire, remote, key, "main", {{"heads/main", root}}, false, {},
                                      options);
    } else if (agent) {
      net::NativeSync sync(plain, fs::current_path());
      auto transport = net::sealed_native_sync(wire, remote, key);
      (void)sync.tick(plain, *transport, {}, options);
    } else {
      require(std::string_view(argv[1]) == "fetch", "invalid sealed probe operation");
      (void)net::fetch_sealed_snapshot(wire, plain, remote, key, "main", {}, options);
    }
    require(!armed, "sealed probe did not reach its checkpoint");
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
} // namespace
namespace syrnck::testing {
void io_checkpoint(std::string_view event, const fs::path &target) {
  if (!armed || phase == "transfer" || target.filename() != "refs.json" || target.parent_path() != plain_path)
    return;
  if (agent) {
    if (event != "atomic.published")
      return;
    const auto refs = parse_refs(read_bytes(target, max_object_size), "invalid sealed probe refs");
    bool sealed = false, native = false;
    for (const auto &[name, unused] : refs) {
      (void)unused;
      sealed |= name.starts_with("sealed/checkpoints/") &&
                !name.starts_with("sealed/checkpoints/agent-candidates/");
      native |= name.ends_with("/observed");
    }
    if ((phase == "after-sealed-observation" && sealed && !native) ||
        (phase == "after-native-observation" && native))
      wait_for_parent();
    return;
  }
  if ((phase == "before-checkpoint" && event == "atomic.file_synced") ||
      (phase == "after-checkpoint" && event == "atomic.published"))
    wait_for_parent();
}
} // namespace syrnck::testing
#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
  std::vector<std::string> values;
  for (int i = 0; i < argc; ++i)
    values.push_back(utf8(fs::path(argv[i])));
  std::vector<char *> pointers;
  for (auto &value : values)
    pointers.push_back(value.data());
  return entry(argc, pointers.data());
}
#else
int main(int argc, char **argv) { return entry(argc, argv); }
#endif
