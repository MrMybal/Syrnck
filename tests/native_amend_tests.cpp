#include "internal.hpp"
#include <iostream>
#include <source_location>
#include <sstream>
using namespace syrnck;
namespace {
template <class F> void fails(F function, std::source_location location = std::source_location::current()) {
  bool failed = false;
  try {
    function();
  } catch (const Error &) {
    failed = true;
  }
  require(failed, "expected amendment failure at line " + std::to_string(location.line()));
}
void write(const fs::path &path, std::string_view value) {
  fs::create_directories(path.parent_path());
  atomic_write(path, bytes(value), true);
}
std::string content(const Store &store, const std::string &revision, const std::string &path) {
  std::ostringstream out;
  store.reconstruct(store.metadata(Kind::revision, revision)["files"][path]["manifest"].get<std::string>(),
                    out);
  return out.str();
}
template <class F> void cancelled(const Store &store, F function) {
  const auto before = store.refs();
  std::stop_source stop;
  fails([&] {
    function(OperationOptions{stop.get_token(), [&](const ProgressEvent &event) {
                                if (event.phase == ProgressPhase::publishing && event.path == "refs")
                                  stop.request_stop();
                              }});
  });
  require(stop.stop_requested() && store.refs() == before, "cancelled amendment published references");
}
void scenario(const fs::path &project, std::optional<Key> key, unsigned physical) {
  std::string original, replacement, staged_tip, final;
  Json parents;
  {
    Store store(project / ".syrnck", key, true);
    const auto empty_refs = store.refs();
    fails([&] { store.amend_message("main", std::string(64, '0'), "unborn"); });
    require(store.refs() == empty_refs && store.objects().empty(), "unborn amend wrote objects");
    write(project / "asset.bin", std::string(1024 * 1024 + 17, 'a'));
    write(project / "removed.txt", "original only");
    const auto first = store.commit_directory(project, "main", "root");
    write(project / "asset.bin", "second parent");
    const auto second = store.commit_directory(project, "other", "other root");
    auto merged = store.metadata(Kind::revision, first);
    parents = Json::array({first, second});
    merged["parents"] = parents;
    merged["message"] = "merge fixture";
    original = store.put(Kind::revision, bytes(merged.dump())).first.id;
    store.set_ref("heads/main", {Kind::revision, original});
    store.create_native_tag("before-amend", original);
    if (physical == 1)
      store.pack();
    if (physical == 2)
      store.pack({512 * 1024 * 1024, 3});
    const auto before = store.refs();
    const auto objects = store.objects();
    fails([&] { store.amend_directory(project, "main", first, "wrong tip"); });
    fails([&] { store.amend_message("main", "HEAD", "invalid expected ID"); });
    require(store.refs() == before && store.objects() == objects, "invalid amendment wrote objects");
    store.set_ref("archive/amend/" + original, {Kind::revision, second});
    fails([&] { store.amend_message("main", original, "archive conflict"); });
    require(store.refs().at("heads/main").id == original, "archive conflict moved tip");
    store.remove_ref("archive/amend/" + original);
    cancelled(store, [&](OperationOptions options) {
      store.amend_message("main", original, "cancelled metadata", options);
    });
    write(project / "asset.bin", std::string(1024 * 1024 + 25, 'b'));
    write(project / "added.txt", "replacement only");
    fs::remove(project / "removed.txt");
    cancelled(store, [&](OperationOptions options) {
      auto ingest = store.settings().ingest_options();
      ingest.stop = options.stop;
      ingest.progress = options.progress;
      store.amend_directory(project, "main", original, "cancelled directory", ingest);
    });
    replacement = store.amend_directory(project, "main", original, "replacement");
    const auto replaced = store.metadata(Kind::revision, replacement);
    require(replaced["parents"] == parents && replaced["message"] == "replacement" &&
                !replaced["files"].contains("removed.txt") && replaced["files"].contains("added.txt") &&
                store.refs().at("archive/amend/" + original).id == original &&
                store.resolve_native_revision("tags/before-amend") == original &&
                store.metadata(Kind::revision, original) == merged,
            "amendment changed parents, tags, archive or original metadata");
    write(project / "asset.bin", "prepared version");
    store.stage_directory(project, "main", {"asset.bin"});
    write(project / "asset.bin", "later working version");
    const auto staged_refs = store.refs();
    fails([&] { store.amend_directory(project, "main", replacement, "index must be explicit"); });
    fails([&] { store.amend_message("main", replacement, "index must be explicit"); });
    require(store.refs() == staged_refs, "non-staged amend disturbed an index");
    cancelled(store, [&](OperationOptions options) {
      store.amend_staged("main", replacement, "cancelled staged", options);
    });
    staged_tip = store.amend_staged("main", replacement, "prepared replacement");
    require(!store.refs().contains("staging/main") &&
                store.refs().at("archive/amend/" + replacement).id == replacement &&
                content(store, staged_tip, "asset.bin") == "prepared version" &&
                text(read_bytes(project / "asset.bin")) == "later working version" &&
                store.metadata(Kind::revision, staged_tip)["parents"] == parents,
            "staged amendment changed workspace, ancestry or prepared bytes");
    // Even an unchanged prepared snapshot can carry a message amendment.
    write(project / "asset.bin", "prepared version");
    store.stage_directory(project, "main", {"."});
    require(store.staged_status("main")["clean"] == true, "clean staging fixture differs");
    final = store.amend_staged("main", staged_tip, "clean staged message");
    const auto files = store.metadata(Kind::revision, final)["files"];
    fs::remove(project / "asset.bin");
    // Message mode and staged mode have no dependency on readable working files.
    final =
        store.amend_message("main", final, "metadata only", {{}, [&](const ProgressEvent &event) {
                                                               require(event.phase != ProgressPhase::scanning,
                                                                       "message amendment scanned workspace");
                                                             }});
    require(store.metadata(Kind::revision, final)["files"] == files &&
                store.metadata(Kind::revision, final)["parents"] == parents,
            "message amendment changed the snapshot");
    // A stale prepared baseline must not be silently retargeted.
    write(project / "asset.bin", "stale version");
    store.stage_directory(project, "main", {"asset.bin"});
    store.set_ref("heads/main", {Kind::revision, replacement});
    const auto stale_refs = store.refs();
    fails([&] { store.amend_staged("main", replacement, "stale"); });
    require(store.refs() == stale_refs, "stale amendment consumed staging");
    store.reset_stage("main");
    store.set_ref("heads/main", {Kind::revision, final});
    const auto pending = store.put(Kind::chunk, bytes("pending checkout fixture")).first;
    store.set_ref("checkouts/active", pending);
    const auto pending_refs = store.refs();
    fails([&] { store.amend_message("main", final, "pending"); });
    fails([&] { store.amend_staged("main", final, "pending"); });
    fails([&] { store.amend_directory(project, "main", final, "pending"); });
    require(store.refs() == pending_refs, "pending checkout allowed amendment");
    store.remove_ref("checkouts/active");
    for (const auto &[name, value] : store.refs()) {
      (void)value;
      if (name != "heads/main" && !name.starts_with("archive/amend/"))
        store.remove_ref(name);
    }
    store.gc(true);
  }
  Store reopened(project / ".syrnck", key);
  require(content(reopened, original, "asset.bin") == std::string(1024 * 1024 + 17, 'a') &&
              content(reopened, replacement, "asset.bin") == std::string(1024 * 1024 + 25, 'b') &&
              content(reopened, staged_tip, "asset.bin") == "prepared version" &&
              content(reopened, final, "asset.bin") == "prepared version" && reopened.fsck() > 0,
          "GC or reopening lost an amendment archive");
}
void missing_payload(const fs::path &project, bool prepared) {
  std::string old, chunk;
  {
    Store store(project / ".syrnck", {}, true);
    write(project / "asset.bin", "old retained bytes");
    old = store.commit_directory(project, "main", "old");
    auto selected = old;
    if (prepared) {
      write(project / "asset.bin", "prepared retained bytes");
      selected = store.stage_directory(project, "main", {"asset.bin"});
    }
    const auto manifest =
        store.metadata(Kind::revision, selected)["files"]["asset.bin"]["manifest"].get<std::string>();
    chunk = store.metadata(Kind::manifest, manifest)["blocks"][0]["id"].get<std::string>();
  }
  require(fs::remove(project / ".syrnck/objects/chunk" / chunk.substr(0, 2) / chunk), "fixture chunk absent");
  Store store(project / ".syrnck");
  const auto before = store.refs();
  if (prepared)
    fails([&] { store.amend_staged("main", old, "missing prepared payload"); });
  else {
    write(project / "asset.bin", "new workspace cannot conceal a broken archive");
    fails([&] { store.amend_directory(project, "main", old, "missing archived payload"); });
    fails([&] { store.amend_message("main", old, "missing retained payload"); });
  }
  require(store.refs() == before, "missing payload published amendment roots");
}
} // namespace
int main() {
  const auto root = fs::temp_directory_path() / ("syrnck-native-amend-" + hex(random_bytes(16)));
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
          key->fill(41);
        }
        scenario(root / (std::to_string(encrypted) + '-' + std::to_string(physical)), key, physical);
      }
    missing_payload(root / "missing-old", false);
    missing_payload(root / "missing-prepared", true);
    std::cout << "native amendment, ancestry, atomic retention, cancellation and GC passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
