#include "watch_journal_fixture.hpp"
#include "watch_journal.hpp"
#include <algorithm>
#include <iostream>
#include <sstream>
using namespace syrnck;
namespace {
template<class F> void fails(F action) {
  bool failed = false;
  try { action(); } catch (const Error &) { failed = true; }
  require(failed, "expected watch journal rejection");
}
void scenario(const fs::path &root, bool encrypted) {
  const auto key = watch_fixture::key(encrypted);
  const auto path = root / "watch-journal";
  Json pending = watch_fixture::record(), completed = watch_fixture::record(true);
  Bytes valid;
  {
    Store store(root, key, true);
    require(store.workspace_watch_journal().is_null(), "missing journal is not null");
    require(store.clear_workspace_watch_journal()["state"] == "absent", "missing journal clear changed meaning");
    std::istringstream retained("retained-before-and-after-journal");
    const auto manifest = store.ingest(retained).first;
    store.set_ref("pins/journal-fixture", {Kind::manifest, manifest});
    const auto refs = store.refs();
    const auto objects = store.objects();
    store.write_workspace_watch_journal(pending);
    require(store.workspace_watch_journal() == pending, "pending journal changed during round trip");
    store.write_workspace_watch_journal(completed);
    require(store.workspace_watch_journal() == completed, "completed journal changed during round trip");
    valid = read_bytes(path);
    if (encrypted) {
      const auto name = std::string("private-asset-name.uasset");
      require(std::search(valid.begin(), valid.end(), name.begin(), name.end()) == valid.end(),
              "encrypted journal exposes asset names");
    }
    std::vector<Json> malformed;
    auto bad = pending; bad["extra"] = 1; malformed.push_back(bad);
    bad = pending; bad["generation"] = 0; malformed.push_back(bad);
    bad = pending; bad["generation"] = -1; malformed.push_back(bad);
    bad = pending; bad["version"] = 2; malformed.push_back(bad);
    bad = pending; bad["workspace"] = "invalid"; malformed.push_back(bad);
    bad = pending; bad["state"] = "observed"; malformed.push_back(bad);
    bad = completed; bad["observation"]["digest"] = "invalid"; malformed.push_back(bad);
    bad = pending; bad["previous"]["state"] = "pending"; malformed.push_back(bad);
    bad = pending; bad["batch"]["overflow"] = true; malformed.push_back(bad);
    bad = pending; bad["batch"]["paths"] = Json::array({"../outside"}); malformed.push_back(bad);
    bad = pending; bad["batch"]["paths"] = Json::array({"z", "a"}); malformed.push_back(bad);
    bad = pending; bad["batch"]["paths"] = Json::array({std::string(1024 * 1024, 'x')}); malformed.push_back(bad);
    for (const auto &value : malformed) {
      fails([&] { store.write_workspace_watch_journal(value); });
      require(read_bytes(path) == valid, "invalid journal write changed checkpoint");
    }
    for (bool clear : {false, true}) {
      std::stop_source stop;
      fails([&] {
        OperationOptions options{stop.get_token(), [&](const ProgressEvent &e) {
          if (e.phase == ProgressPhase::publishing) stop.request_stop();
        }};
        if (clear) store.clear_workspace_watch_journal(options);
        else store.write_workspace_watch_journal(pending, options);
      });
      require(read_bytes(path) == valid, "cancelled journal publication changed checkpoint");
    }
    fails([&] {
      store.write_workspace_watch_journal(pending, {{}, [&](const ProgressEvent &e) {
        if (e.phase == ProgressPhase::publishing) atomic_write(path, bytes("changed"), true);
      }});
    });
    require(text(read_bytes(path)) == "changed", "journal observer change was overwritten");
    atomic_write(path, valid, true);
    {
      auto session_lock = lock_watch_journal(root);
      fails([&] { store.clear_workspace_watch_journal(); });
      require(store.workspace_watch_journal() == completed, "live journal cannot be inspected");
    }
    require(store.refs() == refs && store.objects() == objects, "local journal altered retained roots or objects");
    store.gc(true);
    store.pack();
    require(store.workspace_watch_journal() == completed, "GC or packing removed local checkpoint");
  }
  Store reopened(root, key);
  require(reopened.workspace_watch_journal() == completed, "journal did not survive Store reopen");
  for (unsigned damage = 0; damage != 3; ++damage) {
    auto corrupt = valid;
    if (damage == 0) corrupt.resize(50);
    else if (damage == 1) corrupt.back() ^= 1;
    else corrupt[4] = corrupt[4] == '0' ? '1' : '0';
    atomic_write(path, corrupt, true);
    fails([&] { reopened.workspace_watch_journal(); });
    require(read_bytes(path) == corrupt, "journal inspection changed corrupt bytes");
  }
  require(reopened.clear_workspace_watch_journal()["state"] == "cleared", "corrupt journal was not clearable");
  fs::create_directory(path);
  fails([&] { reopened.workspace_watch_journal(); });
  fails([&] { reopened.write_workspace_watch_journal(pending); });
  fails([&] { reopened.clear_workspace_watch_journal(); });
  require(fs::is_directory(path), "obstructing journal directory was removed");
}
} // namespace
int main(int argc, char **argv) {
  if (argc == 3 && std::string_view(argv[1]) == "pending") {
    try {
      Store store(path_from_utf8(argv[2]));
      store.write_workspace_watch_journal(watch_fixture::record());
      return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
  }
  const auto root = fs::temp_directory_path() / ("syrnck-watch-journal-" + hex(random_bytes(16)));
  struct Cleanup { fs::path path; ~Cleanup() { std::error_code error; fs::remove_all(path, error); } } cleanup{root};
  try {
    for (bool encrypted : {false, true}) scenario(root / std::to_string(encrypted), encrypted);
    std::cout << "watch journal: plain/encrypted validation, isolation and recovery passed\n";
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
