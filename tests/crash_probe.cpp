#include "internal.hpp"
#include "pack_storage.hpp"
#include <fstream>
#include <iostream>
#include <sstream>
using namespace syrnck;
namespace {
struct Control {
  std::string phase, kind;
  std::size_t occurrence = 0, seen = 0;
  bool armed = false;
} control;
std::string target_kind(const fs::path &path) {
  auto name = utf8(path.filename());
  if (name == "config.json")
    return "config";
  if (name == "refs.json")
    return "refs";
  if (name == "checkout.state")
    return "checkout";
  if (path.parent_path().filename() == "old")
    return "checkout-backup";
  if (path.parent_path().filename() == "new")
    return "checkout-prepared";
  if (name == "restored.bin")
    return "restore";
  if (name == "exported")
    return "export";
  if (name == "asset.bin" || name == "second.bin" || name == "empty")
    return "export-file";
  if (path.extension() == ".syp")
    return "pack";
  if (path.extension() == ".syi")
    return "index";
  return utf8(path.parent_path().parent_path().filename());
}
void check(bool value, std::string_view why) {
  require(value, why);
}
std::optional<Key> secret(bool encrypted) {
  if (!encrypted)
    return {};
  Key key{};
  key.fill(0x39); // Public deterministic test key, never used outside fixtures.
  return key;
}
std::string block(std::uint64_t seed) {
  std::string data(1024, '\0');
  auto state = seed + 1234567;
  for (auto &c : data) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    c = static_cast<char>(state);
  }
  return data;
}
std::string content(std::size_t version) {
  return block(0) + block(10 + version) + block(20 + version) + block(0) + block(2);
}
ObjectKey ingest(const Store &store, std::size_t version, std::size_t workers = 1) {
  std::istringstream input(content(version));
  return {Kind::manifest, store.ingest(input, Chunking::fixed_size(1024), {.workers = workers}).first};
}
Json revision(const ObjectKey &manifest, const std::optional<ObjectKey> &parent = {}) {
  return Json{{"version", 1},
              {"parents", parent ? Json::array({parent->id}) : Json::array()},
              {"message", "Crash recovery fixture"},
              {"timestamp", 0},
              {"files", Json{{"asset.bin", Json{{"manifest", manifest.id}, {"executable", false}}}}}};
}
ObjectKey put_revision(const Store &store, const ObjectKey &manifest,
                       const std::optional<ObjectKey> &parent = {}) {
  return store.put(Kind::revision, bytes(revision(manifest, parent).dump())).first;
}
fs::path loose(const Store &store, const ObjectKey &key) {
  return store.root() / "objects" / kind_name(key.kind) / key.id.substr(0, 2) / key.id;
}
Json fixture(const fs::path &root) {
  return parse_json(read_bytes(root.parent_path() / "fixture.json"));
}
Store open(const fs::path &root, const Json &info) {
  return Store(root, secret(info.at("encrypted").get<bool>()), false,
               {.decoded_cache_bytes = 0, .verified_cache_entries = 0, .pack_index_cache_bytes = 0});
}
bool replay_operation(std::string_view operation) {
  return operation == "cherry-pick" || operation == "cherry-pick-abort" || operation == "revert" ||
         operation == "revert-abort";
}
bool replay_abort(std::string_view operation) {
  return operation == "cherry-pick-abort" || operation == "revert-abort";
}
bool merge_operation(std::string_view operation) {
  return operation == "merge" || operation == "merge-abort" || operation == "merge-fast-forward" ||
         operation == "merge-fast-forward-abort" || operation == "merge-resolved" ||
         operation == "merge-resolved-abort";
}
bool merge_abort(std::string_view operation) {
  return merge_operation(operation) && operation.ends_with("-abort");
}
NativeMergeOptions merge_settings(const Json &info) {
  NativeMergeOptions settings;
  if (info["operation"].get<std::string>().starts_with("merge-resolved")) {
    settings.resolutions = NativeResolutionSet{"merge",
                                               parse_key(info["old_revision"]).id,
                                               parse_key(info["merge_source"]).id,
                                               parse_key(info["merge_base"]).id,
                                               {},
                                               {}};
    settings.resolutions->files["asset.bin"] = {NativeResolutionChoice::incoming};
  }
  return settings;
}
bool rewrite_operation(std::string_view op) {
  return op == "reset-soft" || op == "reset-soft-index" || op == "reset-mixed" || op == "reset-mixed-index" ||
         op == "squash" || op == "squash-root";
}
void prepare(const fs::path &root, bool encrypted, const std::string &operation) {
  Store store(root, secret(encrypted), true);
  auto old = ingest(store, 0);
  auto tip = put_revision(store, old);
  std::optional<ObjectKey> merge_base;
  if (merge_operation(operation) && !operation.starts_with("merge-fast-forward")) {
    merge_base = put_revision(store, ingest(store, operation.starts_with("merge-resolved") ? 2 : 0));
    tip = put_revision(store, old, merge_base);
  }
  if (operation == "revert" || operation == "revert-abort")
    tip = put_revision(store, old, put_revision(store, ingest(store, 1)));
  if (rewrite_operation(operation)) {
    auto parent = put_revision(store, ingest(store, 1));
    if (operation == "squash")
      parent = put_revision(store, ingest(store, 2), parent);
    tip = put_revision(store, old, parent);
  }
  if (operation == "export") {
    auto value = revision(old);
    std::istringstream empty;
    auto empty_manifest = store.ingest(empty, Chunking::fixed_size(1024)).first;
    value["files"] = Json{{"asset.bin", Json{{"manifest", old.id}, {"executable", false}}},
                          {"empty", Json{{"manifest", empty_manifest}, {"executable", false}}},
                          {"nested/second.bin", Json{{"manifest", old.id}, {"executable", true}}}};
    tip = store.put(Kind::revision, bytes(value.dump())).first;
  }
  store.set_ref("heads/main", tip);
  store.set_ref("archive/baseline", tip);
  auto garbage = ingest(store, 2);
  store.pack(); // A partially reachable pack: shared blocks must survive GC.
  auto marker = store.put(Kind::chunk, bytes("retained packed marker")).first;
  store.set_ref("pins/marker", marker);
  store.pack(); // This wholly retained pack must stay byte-identical during GC.
  store.put(Kind::chunk, bytes("unreachable packed marker"));
  store.pack(); // A wholly unreachable pack, removed by GC without rewriting.
  auto config = parse_json(read_bytes(root / "config.json"));
  Json info{{"encrypted", encrypted},        {"operation", operation},
            {"old_manifest", key_json(old)}, {"old_revision", key_json(tip)},
            {"marker", key_json(marker)},    {"initial_catalog", config["packs"]}};
  if (rewrite_operation(operation)) {
    const auto parent = store.metadata(Kind::revision, tip.id)["parents"][0].get<std::string>();
    info["rewrite_target"] = key_json({Kind::revision, parent});
    info["rewrite_parents"] = store.metadata(Kind::revision, parent)["parents"];
    if (operation.ends_with("-index")) {
      atomic_write(root.parent_path() / "asset.bin", bytes(content(3)), false);
      info["old_index"] =
          key_json({Kind::revision, store.stage_directory(root.parent_path(), "main", {"asset.bin"})});
    }
    atomic_write(root.parent_path() / "asset.bin", bytes(content(4)), true);
  }
  if (merge_operation(operation)) {
    atomic_write(root.parent_path() / "asset.bin", bytes(content(0)), false);
    const auto source = put_revision(store, ingest(store, 1), merge_base ? merge_base : std::optional{tip});
    store.set_ref("pins/merge-source", source);
    info["merge_source"] = key_json(source);
    if (merge_base)
      info["merge_base"] = key_json(*merge_base);
  }
  if (replay_operation(operation)) {
    atomic_write(root.parent_path() / "asset.bin", bytes(content(0)), false);
    const auto source = operation.starts_with("revert") ? tip : put_revision(store, ingest(store, 1), tip);
    store.set_ref("pins/replay-source", source);
    info["replay_source"] = key_json(source);
  }
  if (operation == "checkout" || operation == "checkout-abort" || operation == "switch" ||
      operation == "switch-abort") {
    atomic_write(root.parent_path() / "asset.bin", bytes(content(0)), false);
    const auto next = put_revision(store, ingest(store, 1), tip);
    store.set_ref("pins/checkout-target", next);
    info["new_revision"] = key_json(next);
    if (operation == "switch" || operation == "switch-abort")
      store.set_ref("heads/feature", next);
  }
  if (operation == "stage-add" || operation == "stage-commit" || operation == "stage-reset" ||
      operation == "amend" || operation == "amend-staged") {
    atomic_write(root.parent_path() / "asset.bin", bytes(content(1)), false);
    if (operation != "stage-add" && operation != "amend")
      store.stage_directory(root.parent_path(), "main", {"asset.bin"});
  }
  if (operation == "tag-delete" || operation == "tag-restore") {
    store.create_native_tag("release/v1", tip.id, "Tagged crash fixture");
    if (operation == "tag-restore")
      info["tag_archive"] = store.delete_native_tag("release/v1", tip.id)["archive"];
  }
  if (operation == "append" || operation == "compact") {
    auto next = ingest(store, 1);
    auto next_tip = put_revision(store, next, tip);
    store.set_ref("heads/main", next_tip);
    info["new_manifest"] = key_json(next);
    info["new_revision"] = key_json(next_tip);
    store.put(Kind::chunk, bytes("loose extra for pack batching"));
  }
  if (operation == "gc") {
    ingest(store, 3); // Unreachable loose metadata and its new chunks.
    store.put(Kind::chunk, bytes("loose garbage one"));
    store.put(Kind::chunk, bytes("loose garbage two"));
  }
  if (operation == "append" || operation == "compact" || operation == "gc") {
    PackStorage packs(root, 0);
    packs.load(config["packs"]);
    // A dead manifest duplicate exposes interrupted GC's incomplete orphan
    // cleanup; ordinary packing must preserve both its blocks and its metadata.
    auto duplicate = operation == "gc" ? garbage : old;
    atomic_write(loose(store, duplicate), packs.read(duplicate), false);
  }
  info["objects"] = store.objects().size();
  info["refs"] = parse_json(read_bytes(root / "refs.json"));
  std::cout << info.dump() << '\n';
}
void perform(Store &store, const Json &info) {
  auto op = info.at("operation").get<std::string>();
  if (op == "commit") {
    auto next = ingest(store, 1, 4);
    auto tip = put_revision(store, next, parse_key(info.at("old_revision")));
    store.set_ref("heads/main", tip);
  } else if (rewrite_operation(op)) {
    const auto old = parse_key(info["old_revision"]);
    if (store.refs().at("heads/main") == old) {
      if (op.starts_with("reset"))
        store.reset_branch("main", old.id, parse_key(info["rewrite_target"]).id,
                           op.starts_with("reset-soft") ? NativeResetMode::soft : NativeResetMode::mixed);
      else
        store.squash_commits("main", old.id, 2, "Squashed crash fixture");
    }
  } else if (op == "amend" || op == "amend-staged" || op == "amend-message") {
    const auto old = parse_key(info.at("old_revision"));
    if (store.refs().at("heads/main") == old) {
      if (op == "amend")
        store.amend_directory(store.root().parent_path(), "main", old.id, "Amended crash fixture");
      else if (op == "amend-staged")
        store.amend_staged("main", old.id, "Amended crash fixture");
      else
        store.amend_message("main", old.id, "Amended crash fixture");
    }
  } else if (merge_operation(op)) {
    if (merge_abort(op))
      store.recover_checkout(true);
    else if (store.checkout_status()["pending"].get<bool>())
      store.recover_checkout();
    else if (store.refs().at("heads/main") == parse_key(info.at("old_revision")))
      store.merge_directory(store.root().parent_path(), "main", parse_key(info["old_revision"]).id,
                            parse_key(info["merge_source"]).id, "Merged crash fixture", merge_settings(info));
  } else if (replay_operation(op)) {
    if (replay_abort(op))
      store.recover_checkout(true);
    else if (store.checkout_status()["pending"].get<bool>())
      store.recover_checkout();
    else if (store.refs().at("heads/main") == parse_key(info.at("old_revision")))
      store.replay_directory(store.root().parent_path(), "main", parse_key(info.at("old_revision")).id,
                             parse_key(info.at("replay_source")).id,
                             op == "revert" ? NativeReplayKind::revert : NativeReplayKind::cherry_pick,
                             "Replayed crash fixture");
  } else if (op == "stage-add") {
    store.stage_directory(store.root().parent_path(), "main", {"asset.bin"});
  } else if (op == "stage-commit") {
    if (store.refs().contains("staging/main"))
      store.commit_staged("main", "Staged crash fixture");
  } else if (op == "stage-reset") {
    store.reset_stage("main");
  } else if (op == "checkout") {
    if (store.checkout_status()["pending"].get<bool>())
      store.recover_checkout();
    else if (store.refs().at("heads/main") != parse_key(info.at("new_revision")))
      store.checkout_directory(store.root().parent_path(), "main", parse_key(info.at("new_revision")).id);
  } else if (op == "checkout-abort") {
    store.recover_checkout(true);
  } else if (op == "switch") {
    if (store.checkout_status()["pending"].get<bool>())
      store.recover_checkout();
    else if (store.native_head()["branch"] != "feature")
      store.switch_directory(store.root().parent_path(), "feature");
  } else if (op == "switch-abort") {
    store.recover_checkout(true);
  } else if (op == "tag-create") {
    if (!store.refs().contains("tags/release/v1"))
      store.create_native_tag("release/v1", parse_key(info.at("old_revision")).id, "Tagged crash fixture");
  } else if (op == "tag-delete") {
    if (store.refs().contains("tags/release/v1"))
      store.delete_native_tag("release/v1", parse_key(info.at("old_revision")).id);
  } else if (op == "tag-restore") {
    if (!store.refs().contains("tags/release/v1"))
      store.restore_native_tag(info.at("tag_archive").get<std::string>());
  } else if (op == "detach") {
    store.detach_history("main", "Detached crash fixture");
  } else if (op == "append" || op == "compact") {
    store.pack({.max_objects_per_pack = 4, .compact = op == "compact"});
  } else if (op == "gc") {
    store.gc(true);
  } else if (op == "restore") {
    auto destination = store.root().parent_path() / "restored.bin";
    if (!fs::exists(destination))
      store.restore_file(parse_key(info.at("old_manifest")).id, destination);
    check(text(read_bytes(destination)) == content(0), "restored bytes differ");
  } else if (op == "export") {
    auto destination = store.root().parent_path() / "exported";
    if (!fs::exists(destination))
      store.export_revision(parse_key(info.at("old_revision")).id, destination);
    check(text(read_bytes(destination / "asset.bin")) == content(0), "exported bytes differ");
  } else {
    throw Error("unknown crash fixture operation");
  }
}
Json verify(const fs::path &root, const Json &info, bool resumed) {
  auto store = open(root, info); // A fresh process must immediately acquire the lock.
  auto refs = store.refs();
  auto old = parse_key(info.at("old_revision"));
  check(refs.at("archive/baseline") == old && refs.at("pins/marker") == parse_key(info.at("marker")),
        "retained references changed");
  std::vector<ObjectKey> roots;
  for (const auto &[name, key] : refs) {
    (void)name;
    roots.push_back(key);
  }
  auto live = store.closure(roots, true);
  auto head = store.metadata(Kind::revision, refs.at("heads/main").id);
  auto manifest = head["files"]["asset.bin"]["manifest"].get<std::string>();
  std::ostringstream current, archived;
  store.reconstruct(manifest, current);
  store.reconstruct(parse_key(info.at("old_manifest")).id, archived);
  check(archived.str() == content(0), "archived revision was lost");
  check(current.str() == content(0) || current.str() == content(1), "current revision is incomplete");
  auto op = info.at("operation").get<std::string>();
  if (rewrite_operation(op)) {
    const bool changed = refs.at("heads/main") != old;
    const auto name = "archive/" + std::string(op.starts_with("reset") ? "reset/" : "squash/") + old.id;
    check(refs.contains(name) == changed && (!changed || refs.at(name) == old),
          "history rewrite published apart from its archive");
    const auto original_index =
        info.contains("old_index") ? std::optional{parse_key(info["old_index"])} : std::nullopt;
    if (original_index) {
      const auto saved = "archive/reset-index/" + original_index->id;
      check(refs.contains(saved) == changed && (!changed || refs.at(saved) == *original_index),
            "history rewrite lost its prepared archive");
    }
    if (changed) {
      if (op.starts_with("reset")) {
        check(refs.at("heads/main") == parse_key(info["rewrite_target"]), "reset selected another target");
        const bool soft = op.starts_with("reset-soft");
        check(refs.contains("staging/main") == soft, "reset published a mixed branch/index state");
        if (soft) {
          const auto prepared = store.metadata(Kind::revision, refs.at("staging/main").id);
          const auto previous = store.metadata(Kind::revision, original_index ? original_index->id : old.id);
          check(prepared["parents"] == Json::array({parse_key(info["rewrite_target"]).id}) &&
                    prepared["files"] == previous["files"],
                "soft reset lost the prepared entries or baseline");
        }
      } else {
        check(head["parents"] == info["rewrite_parents"] && head["message"] == "Squashed crash fixture" &&
                  head["files"] == store.metadata(Kind::revision, old.id)["files"] &&
                  !refs.contains("staging/main"),
              "squash lost tip entries or parent boundary");
      }
    } else {
      check(refs.contains("staging/main") == original_index.has_value() &&
                (!original_index || refs.at("staging/main") == *original_index),
            "unpublished rewrite changed staging");
    }
    check(text(read_bytes(root.parent_path() / "asset.bin")) == content(4),
          "history rewrite touched working bytes");
    if (resumed)
      check(changed, "history rewrite did not resume");
  }
  if (op == "amend" || op == "amend-staged" || op == "amend-message") {
    const bool changed = refs.at("heads/main") != old;
    const auto archive = refs.find("archive/amend/" + old.id);
    check((archive != refs.end()) == changed && (!changed || archive->second == old),
          "amendment published apart from its archive");
    check(refs.contains("staging/main") == (op == "amend-staged" && !changed),
          "amendment consumed staging apart from its branch");
    if (changed)
      check(head["parents"] == store.metadata(Kind::revision, old.id)["parents"] &&
                head["message"] == "Amended crash fixture",
            "amendment ancestry or message changed");
    check(current.str() == content(changed && op != "amend-message" ? 1 : 0),
          "amendment lost snapshot bytes");
    if (resumed)
      check(changed, "amendment did not resume");
  }
  if (op == "tag-create" || op == "tag-delete" || op == "tag-restore") {
    const bool tagged = refs.contains("tags/release/v1");
    check(tagged == refs.contains("tag-metadata/release/v1"), "tag and annotation published separately");
    if (tagged) {
      const auto tag = store.native_tag("release/v1", true)["tag"];
      check(tag["revision"] == old.id && tag["message"] == "Tagged crash fixture", "tag metadata changed");
    }
    unsigned archives = 0;
    for (const auto &[name, key] : refs) {
      (void)key;
      if (name.starts_with("archive/tags/") && name.ends_with("/record")) {
        const auto id = name.substr(13, 64);
        const auto archive = store.native_tag_archive(id, true);
        check(archive["tag"]["revision"] == old.id && archive["tag"]["message"] == "Tagged crash fixture",
              "tag archive lost its target or annotation");
        ++archives;
      }
    }
    check(archives == (op == "tag-restore" || (op == "tag-delete" && !tagged) ? 1u : 0u),
          "tag deletion archive published apart from removal");
    if (resumed)
      check(tagged == (op != "tag-delete"), "tag operation did not resume");
  }
  if (op == "checkout" || op == "checkout-abort" || op == "switch" || op == "switch-abort" ||
      replay_operation(op) || merge_operation(op)) {
    const auto status = store.checkout_status();
    const auto working = root.parent_path() / "asset.bin";
    if (fs::exists(working))
      check(text(read_bytes(working)) == content(0) || text(read_bytes(working)) == content(1),
            "checkout published incomplete working bytes");
    else {
      check(status["pending"].get<bool>(), "checkout lost a working file without a journal");
      const auto backup = path_from_utf8(status["data_directory"].get<std::string>()) / "old/0.bin";
      check(fs::exists(backup) && text(read_bytes(backup)) == content(0), "checkout lost its source backup");
    }
    if (resumed) {
      check(!status["pending"].get<bool>() && fs::exists(working) &&
                text(read_bytes(working)) == content(op == "checkout" || op == "switch" ||
                                                             (replay_operation(op) && !replay_abort(op)) ||
                                                             (merge_operation(op) && !merge_abort(op))
                                                         ? 1
                                                         : 0),
            "checkout recovery did not finish");
      check(current.str() == content(op == "checkout" || (replay_operation(op) && !replay_abort(op)) ||
                                             (merge_operation(op) && !merge_abort(op))
                                         ? 1
                                         : 0),
            "checkout recovery moved the wrong branch");
    }
    if (op == "switch" || op == "switch-abort") {
      check(refs.at("heads/main") == old && refs.at("heads/feature") == parse_key(info.at("new_revision")),
            "switch changed a branch revision");
      const auto head = store.native_head();
      const bool committed = refs.contains("workspace/HEAD");
      check(head["branch"] == (committed ? "feature" : "main") &&
                (!status["pending"].get<bool>() || (status["state"] == "committed") == committed),
            "switch published HEAD apart from its committed journal");
      if (resumed)
        check(committed == (op == "switch"), "switch recovery selected the wrong HEAD");
    }
    if (replay_operation(op) && refs.at("heads/main") != old)
      check(head["parents"] == Json::array({old.id}) && head["message"] == "Replayed crash fixture",
            "replay recovery changed ancestry or commit message");
    if (merge_operation(op)) {
      const auto source = parse_key(info["merge_source"]);
      check(refs.at("pins/merge-source") == source, "merge changed its source pin");
      if (refs.at("heads/main") != old) {
        if (op.starts_with("merge-fast-forward"))
          check(refs.at("heads/main") == source, "fast-forward created another revision");
        else
          check(head["parents"] == Json::array({old.id, source.id}) &&
                    head["message"] == "Merged crash fixture",
                "merge lost a parent or its message");
      }
    }
  }
  if (op == "stage-add" || op == "stage-commit" || op == "stage-reset") {
    const bool indexed = refs.contains("staging/main");
    if (indexed) {
      const auto report = store.staged_status("main");
      check(report["base"] == old.id && report["stale"] == false && current.str() == content(0),
            "staged publication left mixed branch/index state");
      const auto staged = store.metadata(Kind::revision, refs.at("staging/main").id);
      std::ostringstream prepared;
      store.reconstruct(staged["files"]["asset.bin"]["manifest"].get<std::string>(), prepared);
      check(prepared.str() == content(1), "prepared staging bytes were lost");
    }
    if (resumed) {
      check(indexed == (op == "stage-add"), "staging operation did not resume");
      check(current.str() == content(op == "stage-commit" ? 1 : 0), "staged commit bytes differ");
      if (op == "stage-commit")
        check(head["parents"] == Json::array({old.id}) && head["message"] == "Staged crash fixture",
              "staged commit ancestry differs");
    }
  }
  if (resumed) {
    if (op == "commit" || op == "append" || op == "compact")
      check(current.str() == content(1), "new revision was not recovered");
    if (op == "detach") {
      check(head["parents"].empty() && refs.at("archive/" + old.id) == old,
            "detached history lost its archive");
    }
  }
  bool audit_ok = true;
  std::size_t objects = 0;
  try {
    objects = store.fsck();
  } catch (const MissingObject &) {
    audit_ok = false;
  }
  if (resumed || op != "gc")
    check(audit_ok, "complete audit failed");
  if (resumed && op == "gc")
    check(objects == live.size() && store.gc()["unreachable_objects"] == 0,
          "resumed GC left unreachable objects");
  if (op == "append" || op == "compact")
    check(objects == info.at("objects").get<std::size_t>(), "packing lost logical objects");
  if (op == "restore") {
    auto destination = root.parent_path() / "restored.bin";
    if (fs::exists(destination))
      check(text(read_bytes(destination)) == content(0), "partial output published");
    if (resumed)
      check(fs::exists(destination), "restoration did not resume");
  }
  if (op == "export") {
    auto destination = root.parent_path() / "exported";
    if (fs::exists(destination)) {
      check(fs::is_directory(destination) && text(read_bytes(destination / "asset.bin")) == content(0) &&
                text(read_bytes(destination / "nested/second.bin")) == content(0) &&
                fs::file_size(destination / "empty") == 0,
            "incomplete export directory published");
      std::size_t files = 0;
      for (const auto &entry : fs::recursive_directory_iterator(destination))
        files += entry.is_regular_file();
      check(files == 3, "unexpected export content");
    }
    if (resumed)
      check(fs::exists(destination), "export did not resume");
  }
  return Json{{"retained_files_verified", true},
              {"audit_ok", audit_ok},
              {"live_objects", live.size()},
              {"objects", objects},
              {"refs", parse_json(read_bytes(root / "refs.json"))}};
}
} // namespace
namespace syrnck::testing {
void io_checkpoint(std::string_view phase, const fs::path &target) {
  if (!control.armed || phase != control.phase || target_kind(target) != control.kind ||
      ++control.seen != control.occurrence)
    return;
  control.armed = false;
  std::cout << Json{{"protocol", "syrnck-test-io-checkpoint-v1"},
                    {"phase", phase},
                    {"kind", control.kind},
                    {"occurrence", control.seen},
                    {"target", utf8(target)}}
                   .dump()
            << std::endl;
  std::string reply;
  check(static_cast<bool>(std::getline(std::cin, reply)) && reply == "continue", "checkpoint control closed");
}
} // namespace syrnck::testing
int entry(int argc, char **argv) {
  try {
    check(argc >= 3, "usage: crash_probe prepare|run|verify|resume ROOT ...");
    std::string mode = argv[1];
    auto root = path_from_utf8(argv[2]);
    if (mode == "prepare") {
      check(argc == 5, "missing fixture options");
      prepare(root, std::string_view(argv[3]) == "encrypted", argv[4]);
    } else {
      auto info = fixture(root);
      if (mode == "run") {
        check(argc == 6, "missing checkpoint options");
        auto store = open(root, info);
        struct Sentinel {
          fs::path path;
          ~Sentinel() {
            std::ofstream(path, std::ios::binary) << "destructor ran";
          }
        } sentinel{root.parent_path() / "unwound.txt"};
        if (info["operation"] == "checkout-abort" || info["operation"] == "switch-abort" ||
            replay_abort(info["operation"].get<std::string>()) ||
            merge_abort(info["operation"].get<std::string>())) {
          std::stop_source stop;
          try {
            const OperationOptions options{stop.get_token(), [&](const ProgressEvent &event) {
                                             if (event.phase == ProgressPhase::exporting && event.bytes > 0)
                                               stop.request_stop();
                                           }};
            if (merge_abort(info["operation"].get<std::string>()))
              store.merge_directory(root.parent_path(), "main", parse_key(info["old_revision"]).id,
                                    parse_key(info["merge_source"]).id, "Merged crash fixture",
                                    merge_settings(info), options);
            else if (replay_abort(info["operation"].get<std::string>()))
              store.replay_directory(root.parent_path(), "main", parse_key(info.at("old_revision")).id,
                                     parse_key(info.at("replay_source")).id,
                                     info["operation"] == "revert-abort" ? NativeReplayKind::revert
                                                                         : NativeReplayKind::cherry_pick,
                                     "Replayed crash fixture", {}, options);
            else if (info["operation"] == "switch-abort")
              store.switch_directory(root.parent_path(), "feature", options);
            else
              store.checkout_directory(root.parent_path(), "main", parse_key(info.at("new_revision")).id,
                                       options);
          } catch (const Cancelled &) {
          }
          check(store.checkout_status()["state"] == "applying", "missing abort fixture journal");
        }
        control = Control{argv[3], argv[4], static_cast<std::size_t>(std::stoull(argv[5])), 0, true};
        perform(store, info);
        std::cout << Json{{"completed", true}}.dump() << '\n';
      } else {
        check(mode == "verify" || mode == "resume", "unknown probe mode");
        if (mode == "resume") {
          auto store = open(root, info);
          perform(store, info);
        }
        std::cout << verify(root, info, mode == "resume").dump() << '\n';
      }
    }
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
  std::vector<std::string> values;
  for (int i = 0; i < argc; ++i)
    values.push_back(utf8(argv[i]));
  std::vector<char *> pointers;
  for (auto &value : values)
    pointers.push_back(value.data());
  return entry(argc, pointers.data());
}
#else
int main(int argc, char **argv) {
  return entry(argc, argv);
}
#endif
