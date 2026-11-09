#include "native_history_fixture.hpp"
#include "working_file_stamp.hpp"
#include <syrnck/workspace.hpp>
#include <syrnck/watcher.hpp>
#include <syrnck/signing.hpp>
#include <fstream>
#include <iostream>
#include <thread>
using namespace native_test;
namespace {
struct Sample {
  fs::path primary, directory;
  std::optional<Key> key;
  std::string context, target = "main", hidden;
  std::unique_ptr<WorkspaceCapture> capture;
  std::unique_ptr<WorkspaceObservation> observation;
  std::uint64_t bytes = 0;
  Sample(const fs::path &root, bool encrypted, unsigned scope) : primary(root / "primary"), directory(primary) {
    if (encrypted) { key.emplace(); key->fill(67); }
    Fixture fixture(primary, key);
    std::string asset(256 * 1024, 'a');
    Json files = Json::object();
    for (unsigned n = 0; n < 24; ++n) {
      asset.front() = static_cast<char>('a' + n);
      files["Assets/asset" + std::to_string(n) + ".bin"] = fixture.file(asset, false, 64 * 1024);
    }
    files["Source/main.cpp"] = fixture.file("return 0;\n");
    files["Hidden/data"] = fixture.file("hidden bytes");
    files[".syrnckignore"] = fixture.file("*.keep\n");
    hidden = files["Hidden/data"]["manifest"].get<std::string>();
    fixture.install(fixture.revision(files));
    if (scope) {
      context = "edit"; target = "edit"; directory = root / "linked";
      if (scope == 2) fixture.store.register_partial_native_workspace(primary, context, directory, {"Assets", "Source"});
      else fixture.store.register_native_workspace(primary, context, directory);
      fixture.store.create_native_branch(target, fixture.store.refs().at("heads/main").id);
      fixture.store.use_native_workspace(context);
      fixture.store.switch_directory(directory, target);
      if (scope == 3) { fixture.store.detach_checkout(directory, fixture.store.refs().at("heads/edit").id); target = fixture.store.native_target(); }
    }
    atomic_write(directory / "ignored.keep", syrnck::bytes("ignored"), false);
    // Primary ignore rules are outside the selected prefixes of partial trees.
    // Keep its extra file out of that selection as well.
    capture = std::make_unique<WorkspaceCapture>(directory, fixture.store.root());
    observation = std::make_unique<WorkspaceObservation>(directory, fixture.store.root());
    Key seed; seed.fill(74);
    fixture.store.use_native_identity({"Capture author", "capture@example.invalid", 0}, SigningKey::from_seed(seed));
    auto store_options = fixture.store.settings().ingest_options();
    const auto report = capture->capture(fixture.store, target, "warm capture", store_options, observation.get());
    require(fixture.store.metadata(Kind::revision, report["revision"].get<std::string>())["signature"]["algorithm"] == "ed25519",
            "capture lost native signing policy");
    hidden = fixture.store.metadata(Kind::revision, report["revision"].get<std::string>())["files"]["Hidden/data"]["manifest"].get<std::string>();
    bytes = report["capture"]["source_bytes_read"].get<std::uint64_t>();
    require(bytes >= 6 * 1024 * 1024 && report["capture"]["reused_files"] == 0,
            "first capture reused unverified source bytes");
  }
  void run() {
    Store store(primary / ".syrnck", key); store.use_native_workspace(context);
    auto options = store.settings().ingest_options();
    auto observed = observation->workspace_status(store, target, {});
    require(observed["clean"].get<bool>() && observed["unstaged"]["hashed_bytes"] == 0 &&
        observed["unstaged"]["observation"]["mode"] == "targeted", "capture did not seed a complete observer inventory");
    const auto inspected = inspect_native_file(directory, store, "Assets/asset0.bin", {});
    require(inspected.has_value(), "missing capture fixture");
    const auto stamp = working_file_stamp(*inspected);
    const bool supported = stamp && stamp->reusable;
    // The status index is explicitly disposable and may be poisoned. It must
    // never become a source for mutation's manifest reuse.
    atomic_write(store.root() / "working-index.json", syrnck::bytes("{}"), true);
    atomic_write(directory / "Source/main.cpp", syrnck::bytes("return 1;\n"), true);
    std::uint64_t completed = 0;
    options.progress = [&](const ProgressEvent &event) {
      if (event.phase == ProgressPhase::ingesting && event.complete) completed += event.bytes;
    };
    auto report = capture->capture(store, target, "incremental capture", options, observation.get());
    observed = observation->workspace_status(store, target, {});
    require(observed["clean"].get<bool>() && observed["unstaged"]["hashed_bytes"] == 0,
            "post-capture observation reread working bytes");
    require(report["capture"]["source_bytes_read"] == completed, "capture source counters differ from real ingestion");
    if (supported) {
      require(report["capture"]["source_bytes_read"] == 10 && report["capture"]["reused_files"].get<unsigned>() >= 24 &&
          report["capture"]["source_bytes_reused"].get<std::uint64_t>() >= 6 * 1024 * 1024,
          "small capture reread unrelated assets");
    }
    auto revision = report["revision"].get<std::string>();
    require(store.metadata(Kind::revision, revision)["version"] == 1, "capture retained a closed Store's signing identity");
    require(store.status_directory(directory, revision)["clean"].get<bool>(), "cached capture differs from full content scan");
    const auto metadata = store.metadata(Kind::revision, revision);
    require(metadata["files"]["Hidden/data"]["manifest"] == hidden, "partial capture lost hidden history");
    // Same-size in-place replacement with restored mtime must still be read.
    const auto path = directory / "Assets/asset0.bin";
    const auto modified = fs::last_write_time(path);
    { std::fstream output(path, std::ios::binary | std::ios::in | std::ios::out); output.put('z'); require(output.good(), "cannot edit asset"); }
    fs::last_write_time(path, modified);
    options.progress = {};
    report = capture->capture(store, target, "same time edit", options);
    require(report["capture"]["source_bytes_read"].get<std::uint64_t>() >= 256 * 1024,
            "capture missed same-size same-mtime edit");
    require(store.status_directory(directory, report["revision"].get<std::string>())["clean"].get<bool>(),
            "capture reused stale manifest for edited contents");
    // Enumeration still discovers deletions, renames and file/directory changes.
    fs::remove(directory / "Assets/asset1.bin");
    fs::rename(directory / "Assets/asset2.bin", directory / "Assets/Renamed.bin");
    fs::remove(directory / "Assets/asset3.bin");
    fs::create_directory(directory / "Assets/asset3.bin");
    atomic_write(directory / "Assets/asset3.bin/child", syrnck::bytes("child"), false);
#ifndef _WIN32
    fs::permissions(directory / "Assets/asset4.bin", fs::perms::owner_exec, fs::perm_options::add);
#endif
    report = capture->capture(store, target, "namespace", options);
    require(store.status_directory(directory, report["revision"].get<std::string>())["clean"].get<bool>(),
            "capture missed a namespace or permission change");
    // Failed/cancelled publication cannot establish a reusable cache.
    const auto original = store.refs();
    std::stop_source stop;
    options.stop = stop.get_token();
    options.progress = [&](const ProgressEvent &event) { if (event.phase == ProgressPhase::publishing) stop.request_stop(); };
    fails([&] { capture->capture(store, target, "cancelled", options); });
    require(store.refs() == original, "cancelled capture changed refs");
    options.stop = {}; options.progress = {};
    report = capture->capture(store, target, "after cancellation", options);
    require(report["capture"]["reused_files"] == 0, "failed capture retained source proof");
    // A replaced branch head invalidates manifests, including after GC.
    revision = report["revision"].get<std::string>();
    store.detach_history(target, "replace historical root"); store.gc(true);
    report = capture->capture(store, target, "after history rewrite", options);
    require(report["capture"]["reused_files"] == 0, "rewritten history reused stale source proof");
    store.gc(true);
    report = capture->capture(store, target, "after GC", options);
    require(store.status_directory(directory, report["revision"].get<std::string>())["clean"].get<bool>(), "GC invalidated captured data");
    std::cout << "capture " << (key ? "encrypted" : "plain") << '/' << (context.empty() ? "primary" : target)
              << " strong-stamps=" << supported << " source=" << completed << '/' << bytes << '\n';
  }
};
void corruption(const fs::path &root) {
  Fixture fixture(root);
  fixture.write("asset", "retained asset");
  WorkspaceCapture capture(root, fixture.store.root());
  auto options = fixture.store.settings().ingest_options();
  const auto revision = capture.capture(fixture.store, "main", "first", options)["revision"].get<std::string>();
  std::this_thread::sleep_for(std::chrono::milliseconds(3100));
  const auto manifest = fixture.store.metadata(Kind::revision, revision)["files"]["asset"]["manifest"].get<std::string>();
  const auto object = fixture.store.root() / "objects/manifest" / manifest.substr(0, 2) / manifest;
  const auto envelope = read_bytes(object); auto bad = envelope; bad.back() ^= 1;
  atomic_write(object, bad, true);
  const auto refs = fixture.store.refs();
  fails([&] { capture.capture(fixture.store, "main", "corrupt reused object", options); });
  require(fixture.store.refs() == refs, "capture published corrupt reused closure");
  atomic_write(object, envelope, true);
  capture.capture(fixture.store, "main", "repaired", options);
  // Reentrant publication observers cannot replace an unrelated ref table
  // update. Failed captures discard all source proofs as well.
  const auto before = fixture.store.refs();
  bool changed = false;
  options.progress = [&](const ProgressEvent &event) {
    if (!changed && event.phase == ProgressPhase::publishing && event.path == "refs") {
      changed = true;
      fixture.store.set_ref("observer-root", before.at("heads/main"));
    }
  };
  fails([&] { capture.capture(fixture.store, "main", "stale publication", options); });
  require(changed && fixture.store.refs().at("heads/main") == before.at("heads/main") &&
      fixture.store.refs().contains("observer-root"), "capture overwrote observer's reference update");
  options.progress = {};
  const auto fresh = capture.capture(fixture.store, "main", "after stale publication", options);
  require(fresh["capture"]["reused_files"] == 0, "stale publication retained source proofs");
}
} // namespace
int main() {
  try {
    Temporary temporary;
    std::vector<std::unique_ptr<Sample>> samples;
    for (bool encrypted : {false, true}) for (unsigned scope : {0u, 2u, 3u})
      samples.push_back(std::make_unique<Sample>(temporary.path / (std::to_string(encrypted) + '-' + std::to_string(scope)), encrypted, scope));
    // One age barrier for all fixtures; active/recent files always use full reads.
    std::this_thread::sleep_for(std::chrono::milliseconds(3100));
    for (auto &sample : samples) sample->run();
    corruption(temporary.path / "corruption");
    std::cout << "incremental capture data, guards, cancellation, GC and corruption passed\n";
    return 0;
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
