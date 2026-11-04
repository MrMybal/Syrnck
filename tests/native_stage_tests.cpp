#include "internal.hpp"
#include <fstream>
#include <iostream>
#include <source_location>
#include <sstream>
#include <thread>
using namespace syrnck;
namespace {
template <class F>
void fails(F function, std::string_view expected = {},
           std::source_location location = std::source_location::current()) {
  bool failed = false;
  try {
    function();
  } catch (const Error &error) {
    failed = expected.empty() || error.what() == expected;
    if (!failed)
      std::cerr << "Expected: " << expected << "; received: " << error.what() << '\n';
  }
  require(failed, "expected native staging failure at line " + std::to_string(location.line()));
}
void write(const fs::path &path, std::string_view value) {
  fs::create_directories(path.parent_path());
  atomic_write(path, bytes(value), true);
}
std::string content(const Store &store, std::string_view revision, const std::string &path) {
  const auto entry = store.metadata(Kind::revision, revision)["files"][path];
  std::ostringstream out;
  store.reconstruct(entry["manifest"].get<std::string>(), out);
  return out.str();
}
void layout(const Store &store, unsigned physical) {
  if (physical == 1)
    store.pack();
  if (physical == 2)
    store.pack({512 * 1024 * 1024, 2});
}
void scenario(const fs::path &project, std::optional<Key> key, unsigned physical) {
  std::string baseline, staged;
  const std::string prepared(1024 * 1024 + 3, 'p');
  {
    Store store(project / ".syrnck", key, true);
    require(store.staged_status("main")["snapshot"].is_null(), "new branch has staging");
    write(project / "edited.bin", "old edit");
    write(project / "unchosen.txt", "old unchosen");
    write(project / "dir/removed.txt", "old removal");
    baseline = store.commit_directory(project, "main", "baseline");
    layout(store, physical);
    write(project / "edited.bin", prepared);
    write(project / "unchosen.txt", "unselected working edit");
    fs::remove(project / "dir/removed.txt");
    write(project / "dir/new.txt", "prepared addition");
    staged = store.stage_directory(project, "main", {"edited.bin", "dir"}, {.workers = 3});
    const auto report = store.staged_status("main");
    require(report["base"] == baseline && report["head"] == baseline && report["snapshot"] == staged &&
                report["stale"] == false && report["counts"]["added"] == 1 &&
                report["counts"]["removed"] == 1 && report["counts"]["modified"] == 1 &&
                report["counts"]["unchanged"] == 1,
            "staging selection differs");
    require(content(store, staged, "edited.bin") == prepared &&
                content(store, staged, "unchosen.txt") == "old unchosen",
            "staging included an unselected edit");
    require(store.stage_directory(project, "main", {"dir", "edited.bin", "dir"}) == staged,
            "unchanged staging is not deterministic");
    const auto before = store.refs();
    fails([&] { store.commit_directory(project, "main", "accidental full snapshot"); });
    fails([&] { store.stage_directory(project, "main", {"missing", "edited.bin"}); });
    fails([&] { store.stage_directory(project, "main", {}); });
    for (const auto &path : {"../escape", "/absolute", "a/../b", ".git/config", "dir\\new.txt"})
      fails([&] { store.stage_directory(project, "main", {path}); });
    fails([&] { store.reset_stage("main", {"missing"}); });
    require(store.refs() == before, "failed selection changed references");

    const auto caller = std::this_thread::get_id();
    for (const auto phase : {ProgressPhase::scanning, ProgressPhase::ingesting, ProgressPhase::verifying,
                             ProgressPhase::publishing}) {
      std::stop_source cancel;
      auto options = store.settings().ingest_options();
      options.workers = 3;
      options.stop = cancel.get_token();
      bool seen = false;
      options.progress = [&](const ProgressEvent &event) {
        require(std::this_thread::get_id() == caller, "staging callback changed threads");
        if (event.phase == phase && (phase != ProgressPhase::publishing || event.path == "refs")) {
          seen = true;
          cancel.request_stop();
        }
      };
      fails([&] { store.stage_directory(project, "main", {"edited.bin"}, options); },
            phase == ProgressPhase::ingesting ? "ingestion cancelled" : "operation cancelled");
      require(seen && store.refs() == before, "cancelled staging changed references");
    }
    const auto abort_publication = [](const ProgressEvent &event) {
      if (event.phase == ProgressPhase::publishing)
        throw Error("staging observer sentinel");
    };
    fails([&] { store.commit_staged("main", "cancelled commit", {{}, abort_publication}); },
          "staging observer sentinel");
    fails([&] { store.reset_stage("main", {}, {{}, abort_publication}); }, "staging observer sentinel");
    fails([&] { store.reset_stage("main", {"edited.bin"}, {{}, abort_publication}); },
          "staging observer sentinel");
    require(store.refs() == before, "observer failure changed references");
    bool mutated = false;
    IngestOptions mutation;
    mutation.progress = [&](const ProgressEvent &event) {
      if (!mutated && event.phase == ProgressPhase::ingesting) {
        mutated = true;
        std::ofstream out(project / "edited.bin", std::ios::binary | std::ios::app);
        out.put('!');
      }
    };
    fails([&] { store.stage_directory(project, "main", {"edited.bin"}, mutation); },
          "file changed during native staging; retry operation");
    require(mutated && store.refs() == before, "source mutation changed references");

    std::stop_source stop;
    stop.request_stop();
    fails([&] { store.staged_status("main", {stop.get_token()}); }, "operation cancelled");
    fails([&] { store.reset_stage("main", {}, {stop.get_token()}); }, "operation cancelled");
    fails([&] { store.commit_staged("main", "pre-cancelled", {stop.get_token()}); }, "operation cancelled");
    require(store.refs() == before, "pre-cancelled staging changed references");

    // The staging root alone retains both prepared content and its baseline.
    store.remove_ref("heads/main");
    require(store.staged_status("main")["stale"] == true, "removed head did not stale the index");
    store.gc(true);
    require(content(store, baseline, "edited.bin") == "old edit" &&
                content(store, staged, "edited.bin") == prepared,
            "GC lost staged data or baseline");
    store.set_ref("heads/main", {Kind::revision, baseline});
    layout(store, physical);
  }
  {
    Store store(project / ".syrnck", key);
    require(store.staged_status("main")["snapshot"] == staged, "staging did not survive reopen");
    write(project / "edited.bin", "later edit must stay outside the commit");
    fs::remove(project / "dir/new.txt");
    const auto committed = store.commit_staged("main", "selected changes");
    const auto revision = store.metadata(Kind::revision, committed);
    require(revision["parents"] == Json::array({baseline}) && revision["message"] == "selected changes" &&
                content(store, committed, "edited.bin") == prepared &&
                content(store, committed, "dir/new.txt") == "prepared addition" &&
                content(store, committed, "unchosen.txt") == "old unchosen" &&
                !revision["files"].contains("dir/removed.txt"),
            "staged commit reread working files or changed ancestry");
    require(!store.refs().contains("staging/main") && store.refs().at("heads/main").id == committed,
            "staged commit did not consume the index");
    require(text(read_bytes(project / "edited.bin")) == "later edit must stay outside the commit",
            "staged commit changed working files");
    store.gc(true);
    require(!store.contains({Kind::revision, staged}) && content(store, committed, "edited.bin") == prepared,
            "committed data was lost when collecting the staging snapshot");
    fails([&] { store.commit_staged("main", "no index"); }, "no native staging snapshot");
    store.stage_directory(project, "main", {"edited.bin"});
    const auto restored = store.reset_stage("main", {"edited.bin"});
    require(restored && store.staged_status("main")["clean"] == true, "selective reset differs");
    fails([&] { store.commit_staged("main", "empty selection"); }, "no staged native changes");
    require(!store.reset_stage("main") && store.staged_status("main")["snapshot"].is_null(),
            "full staging reset differs");
    require(store.fsck() > 0, "staging fixture audit failed");
  }
}
void branches(const fs::path &project) {
  Store store(project / ".syrnck", {}, true);
  write(project / "file.txt", "first");
  const auto first = store.stage_directory(project, "feature/topic", {"."});
  require(store.staged_status("main")["snapshot"].is_null(), "staging leaked between branches");
  store.reset_stage("feature/topic", {"."});
  fails([&] { store.commit_staged("feature/topic", "empty unborn"); }, "no staged native changes");
  require(store.stage_directory(project, "feature/topic", {"file.txt"}) == first, "unborn restaging differs");
  const auto committed = store.commit_staged("feature/topic", "unborn");
  require(store.metadata(Kind::revision, committed)["parents"].empty(), "unborn commit has a parent");
  store.set_ref("heads/main", {Kind::revision, committed});
  write(project / "file.txt", "second");
  const auto prepared = store.stage_directory(project, "feature/topic", {"file.txt"});
  const auto advanced = store.commit_directory(project, "main", "independent branch");
  store.set_ref("heads/feature/topic", {Kind::revision, advanced});
  const auto before = store.refs();
  const auto stale = store.staged_status("feature/topic");
  require(stale["stale"] == true && stale["base"] == committed && stale["head"] == advanced &&
              stale["snapshot"] == prepared,
          "stale staging report differs");
  fails([&] { store.commit_staged("feature/topic", "stale commit"); });
  fails([&] { store.stage_directory(project, "feature/topic", {"."}); });
  fails([&] { store.reset_stage("feature/topic", {"file.txt"}); });
  require(store.refs() == before, "stale staging changed references");
  store.reset_stage("feature/topic");
  require(store.staged_status("feature/topic")["stale"] == false, "stale index was not discarded");
  auto invalid = store.metadata(Kind::revision, committed);
  invalid["message"] = "ordinary revision is not an index";
  const auto wrong = store.put(Kind::revision, bytes(invalid.dump())).first;
  store.set_ref("staging/main", wrong);
  fails([&] { store.staged_status("main"); }, "invalid native staging snapshot");
  store.remove_ref("staging/main");
  store.set_ref("staging/main",
                {Kind::manifest, invalid["files"]["file.txt"]["manifest"].get<std::string>()});
  fails([&] { store.stage_directory(project, "main", {"."}); }, "invalid native staging snapshot");
}
void missing_dependencies(const fs::path &project) {
  std::string staged, manifest, chunk;
  {
    Store store(project / ".syrnck", {}, true);
    write(project / "file.bin", "baseline");
    store.commit_directory(project, "main", "baseline");
    write(project / "file.bin", "prepared dependency that will be deliberately removed");
    staged = store.stage_directory(project, "main", {"file.bin"});
    manifest = store.metadata(Kind::revision, staged)["files"]["file.bin"]["manifest"].get<std::string>();
    chunk = store.metadata(Kind::manifest, manifest)["blocks"][0]["id"].get<std::string>();
  }
  // Only this fixture's loose chunk is removed, after closing all handles.
  require(fs::remove(project / ".syrnck/objects/chunk" / chunk.substr(0, 2) / chunk),
          "cannot remove fixture dependency");
  Store store(project / ".syrnck");
  const auto before = store.refs();
  require(store.staged_status("main")["snapshot"] == staged, "metadata inspection needs payloads");
  fails([&] { store.commit_staged("main", "missing payload"); });
  write(project / "other.bin", "a different file cannot conceal a broken retained dependency");
  fails([&] { store.stage_directory(project, "main", {"other.bin"}); });
  require(store.refs() == before, "missing staging dependency changed references");
  store.reset_stage("main", {"file.bin"});
  require(store.staged_status("main")["clean"] == true, "cannot reset a broken prepared file");
  store.gc(true);
  require(store.fsck() > 0, "reset and GC did not recover broken unreferenced staging");
}
} // namespace
int main() {
  const auto root = fs::temp_directory_path() / ("syrnck-native-stage-" + hex(random_bytes(16)));
  struct Cleanup {
    fs::path root;
    ~Cleanup() {
      std::error_code error;
      fs::remove_all(root, error);
    }
  } cleanup{root};
  try {
    for (bool encrypted : {false, true})
      for (unsigned physical = 0; physical != 3; ++physical) {
        std::optional<Key> key;
        if (encrypted) {
          key.emplace();
          key->fill(43);
        }
        scenario(root / (std::to_string(encrypted) + '-' + std::to_string(physical)), key, physical);
      }
    branches(root / "branches");
    missing_dependencies(root / "dependencies");
    std::cout << "native staging tests passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
