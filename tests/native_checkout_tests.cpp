#include "native_workspace.hpp"
#include <fstream>
#include <iostream>
#include <source_location>
#include <sstream>
using namespace syrnck;
namespace {
std::function<void(std::string_view, const fs::path &)> hook;
template <class F> void fails(F function, std::source_location location = std::source_location::current()) {
  bool failed = false;
  try {
    function();
  } catch (const Error &) {
    failed = true;
  }
  require(failed, "expected checkout failure at line " + std::to_string(location.line()));
}
void write(const fs::path &path, std::string_view data) {
  fs::create_directories(path.parent_path());
  atomic_write(path, bytes(data), true);
}
std::string manifest(const Store &store, std::string_view value) {
  std::istringstream input{std::string(value)};
  return store.ingest(input).first;
}
std::string target_revision(const Store &store, const std::string &base, std::string_view value) {
  const auto original = store.metadata(Kind::revision, base);
  Json target{
      {"version", 1},
      {"parents", Json::array({base})},
      {"message", "checkout target"},
      {"timestamp", 0},
      {"files",
       Json{{"asset.bin", Json{{"manifest", manifest(store, value)}, {"executable", false}}},
            {"new/file.txt", Json{{"manifest", manifest(store, "created")}, {"executable", false}}},
            {"script", Json{{"manifest", original["files"]["script"]["manifest"]}, {"executable", true}}}}}};
  return store.put(Kind::revision, bytes(target.dump())).first.id;
}
void layout(const Store &store, unsigned storage) {
  if (storage == 1)
    store.pack();
  if (storage == 2)
    store.pack({512 * 1024 * 1024, 3});
}
void scenario(const fs::path &project, std::optional<Key> key, unsigned storage) {
  const std::string original(1024 * 1024 + 3, 'a'), replacement(original.size(), 'b');
  std::string base, target, id;
  {
    Store store(project / ".syrnck", key, true);
    write(project / "asset.bin", original);
    write(project / "removed.txt", "removed");
    write(project / "script", "#!/bin/sh\n");
    base = store.commit_directory(project, "main", "baseline");
    target = target_revision(store, base, replacement);
    store.set_ref("pins/target", {Kind::revision, target});
    write(project / "untracked", "preserved");
    layout(store, storage);
    std::stop_source stop;
    fails([&] {
      store.checkout_directory(project, "main", target,
                               {stop.get_token(), [&](const ProgressEvent &event) {
                                  if (event.phase == ProgressPhase::reconstructing && event.bytes > 0)
                                    stop.request_stop();
                                }});
    });
    const auto pending = store.checkout_status();
    require(pending["pending"] == true && pending["state"] == "preparing" &&
                store.refs().at("heads/main").id == base &&
                text(read_bytes(project / "asset.bin")) == original,
            "preparation cancellation changed workspace or branch");
    for (bool remove : {false, true})
      fails([&] { store.gc(remove); });
    fails([&] { store.commit_directory(project, "main", "blocked"); });
    fails([&] { store.stage_directory(project, "main", {"."}); });
    fails([&] { store.reset_stage("main"); });
    fails([&] { store.commit_staged("main", "blocked"); });
    fails([&] { store.detach_history("main", "blocked"); });
    require(store.plan_checkout(project, "main", target)["ready"] == false,
            "pending checkout did not block preflight");
    write(project / "asset.bin", "edit after preparation cancellation");
    store.recover_checkout(true);
    require(store.checkout_status()["pending"] == false &&
                text(read_bytes(project / "asset.bin")) == "edit after preparation cancellation",
            "discarding preparation overwrote a working edit");
    write(project / "asset.bin", original);
    std::stop_source applying;
    fails([&] {
      store.checkout_directory(project, "main", target,
                               {applying.get_token(), [&](const ProgressEvent &event) {
                                  if (event.phase == ProgressPhase::exporting && event.bytes > 0)
                                    applying.request_stop();
                                }});
    });
    const auto partial = store.checkout_status();
    id = partial["id"].get<std::string>();
    require(partial["state"] == "applying" && partial["pending"] == true &&
                text(read_bytes(project / "asset.bin")) == replacement &&
                store.refs().at("heads/main").id == base,
            "partial checkout state differs");
    layout(store, storage);
  }
  {
    Store store(project / ".syrnck", key);
    const auto result = store.recover_checkout();
    require(result["state"] == "committed" && result["pending"] == false && result["id"] == id &&
                store.refs().at("heads/main").id == target &&
                store.refs().at("archive/checkout/" + id).id == base &&
                store.refs().at("checkouts/completed/" + id).id == target,
            "checkout did not publish retained branch state");
    require(text(read_bytes(project / "asset.bin")) == replacement && !fs::exists(project / "removed.txt") &&
                text(read_bytes(project / "new/file.txt")) == "created" &&
                text(read_bytes(project / "untracked")) == "preserved",
            "checkout working contents differ");
    require(!fs::exists(path_from_utf8(store.checkout_status(id)["data_directory"].get<std::string>())) &&
                store.checkout_status(id)["state"] == "committed" &&
                store.checkout_status()["pending"] == false,
            "checkout cleanup or receipt differs");
    const auto status = store.status_directory(project, target);
    for (const auto &row : status["changes"])
      require(row["path"] == "untracked", "checkout produced an unexpected tracked status change");
    require(store.checkout_directory(project, "main", target)["state"] == "unchanged",
            "same revision checkout was not a no-op");
    require(store.recover_checkout()["pending"] == false, "completed recovery is not idempotent");
    store.gc(true);
    require(store.metadata(Kind::revision, base)["message"] == "baseline" && store.fsck() > 0,
            "GC lost checkout retention");
    store.checkout_directory(project, "main", base);
    require(text(read_bytes(project / "asset.bin")) == original &&
                text(read_bytes(project / "removed.txt")) == "removed" &&
                !fs::exists(project / "new/file.txt"),
            "checkout round trip differs");

    // A source edited in the exact gap before its backup move must be retained.
    bool changed = false;
    hook = [&](std::string_view phase, const fs::path &path) {
      if (!changed && phase == "move.before" && path.parent_path().filename() == "old") {
        changed = true;
        write(project / "asset.bin", "edit immediately before backup move");
      }
    };
    fails([&] { store.checkout_directory(project, "main", target); });
    hook = {};
    require(changed && store.checkout_status()["pending"] == true, "backup-race fixture did not run");
    store.recover_checkout(true);
    require(text(read_bytes(project / "asset.bin")) == "edit immediately before backup move" &&
                store.refs().at("heads/main").id == base,
            "abort discarded the source edited at backup time");
    write(project / "asset.bin", original);

    std::stop_source partial;
    fails([&] {
      store.checkout_directory(project, "main", target,
                               {partial.get_token(), [&](const ProgressEvent &event) {
                                  if (event.phase == ProgressPhase::exporting && event.bytes > 0)
                                    partial.request_stop();
                                }});
    });
    write(project / "asset.bin", "edit after partial checkout");
    fails([&] { store.recover_checkout(); });
    fails([&] { store.recover_checkout(true); });
    require(text(read_bytes(project / "asset.bin")) == "edit after partial checkout",
            "recovery overwrote a user edit");
    write(project / "rescued", text(read_bytes(project / "asset.bin")));
    write(project / "asset.bin", replacement);
    store.recover_checkout(true);
    require(text(read_bytes(project / "asset.bin")) == original &&
                text(read_bytes(project / "rescued")) == "edit after partial checkout",
            "abort failed after preserving the user edit");

    std::stop_source collision;
    fails([&] {
      store.checkout_directory(project, "main", target,
                               {collision.get_token(), [&](const ProgressEvent &event) {
                                  if (event.phase == ProgressPhase::exporting && event.bytes > 0)
                                    collision.request_stop();
                                }});
    });
    write(project / "new/file.txt", "external destination");
    fails([&] { store.recover_checkout(); });
    store.recover_checkout(true);
    require(text(read_bytes(project / "new/file.txt")) == "external destination" &&
                text(read_bytes(project / "asset.bin")) == original,
            "abort discarded an untracked collision");
    fs::remove(project / "new/file.txt");

    std::stop_source undo_race;
    fails([&] {
      store.checkout_directory(project, "main", target,
                               {undo_race.get_token(), [&](const ProgressEvent &event) {
                                  if (event.phase == ProgressPhase::exporting && event.bytes > 0)
                                    undo_race.request_stop();
                                }});
    });
    bool edited_during_undo = false;
    hook = [&](std::string_view phase, const fs::path &path) {
      if (!edited_during_undo && phase == "move.before" && path.parent_path().filename() == "new") {
        edited_during_undo = true;
        write(project / "asset.bin", "edit immediately before undo move");
      }
    };
    fails([&] { store.recover_checkout(true); });
    hook = {};
    const auto rescued_stage =
        path_from_utf8(store.checkout_status()["data_directory"].get<std::string>()) / "new/0.bin";
    require(edited_during_undo && text(read_bytes(rescued_stage)) == "edit immediately before undo move",
            "undo-move race lost working bytes");
    fails([&] { store.recover_checkout(true); }); // Restores old bytes, retains the changed private file.
    require(store.checkout_status()["state"] == "aborted" &&
                text(read_bytes(project / "asset.bin")) == original,
            "abort cleanup did not retain its completed rollback marker");
    write(project / "rescued-undo", text(read_bytes(rescued_stage)));
    fs::remove(rescued_stage); // Only after preserving this fixture's changed private file.
    store.recover_checkout();
    require(text(read_bytes(project / "rescued-undo")) == "edit immediately before undo move" &&
                store.checkout_status()["pending"] == false,
            "cleanup replay changed restored files");

    // A late observer can change the filesystem, but cannot publish that edit.
    fails([&] {
      store.checkout_directory(project, "main", target,
                               {{}, [&](const ProgressEvent &event) {
                                  if (event.phase == ProgressPhase::publishing && event.path == "checkout")
                                    write(project / "asset.bin", "late edit");
                                }});
    });
    require(store.refs().at("heads/main").id == base &&
                text(read_bytes(project / "asset.bin")) == "late edit",
            "final observer edit escaped revalidation");
    write(project / "asset.bin", replacement);
    store.recover_checkout(true);
    require(text(read_bytes(project / "asset.bin")) == original &&
                store.checkout_status()["pending"] == false,
            "late-edit recovery failed");

    // An exception during committed cleanup must never turn into a rollback.
    bool committed = false;
    hook = [&](std::string_view phase, const fs::path &) {
      if (phase == "checkout.committed") {
        committed = true;
        throw Error("committed checkout fixture");
      }
    };
    fails([&] { store.checkout_directory(project, "main", target); });
    hook = {};
    require(committed && store.checkout_status()["state"] == "committed",
            "committed recovery fixture did not run");
    fails([&] { store.recover_checkout(true); });
    store.recover_checkout();
    require(store.refs().at("heads/main").id == target && store.checkout_status()["pending"] == false,
            "committed checkout cleanup changed the branch");
  }
}
void missing_objects(const fs::path &project) {
  Store store(project / ".syrnck", {}, true);
  write(project / "asset.bin", "original");
  write(project / "script", "script");
  const auto base = store.commit_directory(project, "main", "baseline");
  const auto target = target_revision(store, base, "missing new payload");
  const auto file =
      store.metadata(Kind::revision, target)["files"]["asset.bin"]["manifest"].get<std::string>();
  const auto block = store.metadata(Kind::manifest, file)["blocks"][0]["id"].get<std::string>();
  require(fs::remove(store.root() / "objects/chunk" / block.substr(0, 2) / block),
          "cannot remove checkout fixture chunk");
  const auto refs = store.refs();
  fails([&] { store.checkout_directory(project, "main", target); });
  require(store.refs() == refs && !store.checkout_status()["pending"].get<bool>() &&
              !fs::exists(store.root() / "checkouts") &&
              text(read_bytes(project / "asset.bin")) == "original",
          "missing target objects changed workspace or references");
}
} // namespace
namespace syrnck::testing {
void io_checkpoint(std::string_view phase, const fs::path &path) {
  if (hook)
    hook(phase, path);
}
} // namespace syrnck::testing
int main() {
  const auto root = fs::temp_directory_path() / ("syrnck-checkout-" + hex(random_bytes(16)));
  struct Cleanup {
    fs::path root;
    ~Cleanup() {
      std::error_code error;
      fs::remove_all(root, error);
    }
  } cleanup{root};
  try {
    for (bool encrypted : {false, true})
      for (unsigned storage = 0; storage != 3; ++storage) {
        std::optional<Key> key;
        if (encrypted) {
          key.emplace();
          key->fill(31);
        }
        scenario(root / (std::to_string(encrypted) + '-' + std::to_string(storage)), key, storage);
      }
    missing_objects(root / "missing");
    std::cout << "native checkout, recovery, races and retention passed\n";
  } catch (const std::exception &error) {
    hook = {};
    std::cerr << error.what() << '\n';
    return 1;
  }
}
