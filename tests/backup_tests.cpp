#include "native_history_fixture.hpp"
#include <syrnck/backup.hpp>
#include <syrnck/asset_locks.hpp>
#include <iostream>
using namespace native_test;
namespace {
fs::path object_path(const Store &store, const ObjectKey &key) {
  return store.root() / "objects" / kind_name(key.kind) / key.id.substr(0, 2) / key.id;
}
void snapshots(const fs::path &root, std::optional<Key> source_key, unsigned layout) {
  Fixture f(root / "source", source_key);
  const auto first_file = f.file("original asset bytes"), second_file = f.file("updated asset bytes");
  const auto first = f.revision(Json{{"Content/Map.uasset", first_file}});
  const auto second = f.revision(Json{{"Content/Map.uasset", second_file}}, Json::array({first}));
  f.install(second);
  f.store.set_ref("archive/reset/" + first, {Kind::revision, first});
  f.store.create_native_tag("release", first);
  f.store.create_native_branch("working", second);
  f.store.switch_directory(f.project, "working");
  AssetLocks(f.store).acquire("alice", {"Content/Map.uasset"});
  RepositorySettings settings;
  settings.ingest_workers = 2;
  f.store.set_settings(settings);
  f.pack(layout);
  std::optional<Key> destination_key;
  if (source_key) {
    destination_key.emplace();
    destination_key->fill(92);
    fails([&] { create_backup(f.store, root / "downgrade"); });
    require(!fs::exists(root / "downgrade"), "encrypted backup silently downgraded encryption");
  }
  const auto before = f.physical();
  const auto report = create_backup(f.store, root / "backup", destination_key);
  require(std::find(report["excluded_refs"].begin(), report["excluded_refs"].end(), Json("workspace/HEAD")) !=
              report["excluded_refs"].end(),
          "backup fixture did not omit an explicit workspace binding");
  require(f.physical() == before, "backup modified its source");
  require(report["objects"].get<std::size_t>() > 0 && report["state"] == "created",
          "backup did not publish a complete snapshot");
  Store backup(root / "backup", destination_key);
  const auto verified = verify_backup(backup);
  require(verified["snapshot"] == report["snapshot"], "backup receipt differs from verification");
  auto expected = f.store.refs();
  for (auto i = expected.begin(); i != expected.end();)
    if (i->first == "workspace/HEAD" || i->first.starts_with("checkouts/receipts/"))
      i = expected.erase(i);
    else
      ++i;
  require(backup.refs() == expected && backup.settings().json() == settings.json(),
          "backup lost retained roots or settings");
  require(!backup.refs().contains("workspace/HEAD"), "backup copied an absolute workspace binding");
  const auto restored = restore_backup(backup, root / "restored", destination_key);
  require(restored["state"] == "restored" && !fs::exists(root / "restored/backup.json"),
          "restoration kept immutable backup metadata");
  Store output(root / "restored", destination_key);
  require(output.refs() == expected, "restore changed retained roots");
  output.gc(true);
  std::ostringstream old_bytes, current_bytes;
  output.reconstruct(first_file["manifest"].get<std::string>(), old_bytes);
  output.reconstruct(second_file["manifest"].get<std::string>(), current_bytes);
  require(old_bytes.str() == "original asset bytes" && current_bytes.str() == "updated asset bytes",
          "restored versions did not survive GC");
  fails([&] { restore_backup(backup, root / "restored", destination_key); });
  require(output.refs() == expected, "restore replaced an existing destination");
  backup.put(Kind::chunk, bytes("unexpected additional object"));
  fails([&] { verify_backup(backup); });
}
void repair(const fs::path &root) {
  Fixture f(root / "source");
  const auto file = f.file("retained asset");
  const auto manifest = file["manifest"].get<std::string>();
  const auto revision = f.revision(Json{{"asset", file}});
  f.install(revision);
  create_backup(f.store, root / "backup");
  Store backup(root / "backup");
  // New complete metadata may be absent from the older backup. Only the
  // missing dependencies need coverage there.
  const auto next =
      f.revision(Json{{"asset", file}, {"new.cpp", f.file("new source")}}, Json::array({revision}));
  f.store.set_ref("heads/main", {Kind::revision, next});
  const auto value = f.store.metadata(Kind::manifest, manifest);
  const ObjectKey chunk{Kind::chunk, value["blocks"][0]["id"].get<std::string>()};
  require(fs::remove(object_path(f.store, {Kind::manifest, manifest})) &&
              fs::remove(object_path(f.store, chunk)),
          "repair fixture could not remove retained dependencies");
  f.store.clear_cache();
  const auto broken = f.physical();
  const auto refs = f.store.refs();
  const auto preview = repair_from_backup(f.store, backup);
  require(preview["ready"] == true && preview["missing"].size() == 2 && preview["applied"] == false &&
              f.physical() == broken,
          "repair preview changed missing objects");
  const auto fixed = repair_from_backup(f.store, backup, true);
  require(fixed["repaired"] == 2 && f.store.refs() == refs, "repair changed roots or missed dependencies");
  f.store.fsck();
  require(repair_from_backup(f.store, backup, true)["repaired"] == 0, "repair was not idempotent");
  atomic_write(object_path(f.store, chunk), bytes("corrupt original bytes"), true);
  require(fs::remove(object_path(f.store, {Kind::manifest, manifest})),
          "repair fixture retained its manifest");
  f.store.clear_cache();
  const auto corrupt = f.physical();
  const auto blocked = repair_from_backup(f.store, backup);
  require(blocked["ready"] == false && blocked["corrupt"].size() == 1 && blocked["missing"].size() == 1,
          "repair did not distinguish missing and corrupt objects");
  fails([&] { repair_from_backup(f.store, backup, true); });
  require(f.physical() == corrupt, "blocked repair partially changed storage");
}
void cancellation(const fs::path &root) {
  Fixture f(root / "source");
  const auto revision = f.revision(Json{{"asset", f.file("cancelled asset")}});
  f.install(revision);
  std::stop_source stop;
  stop.request_stop();
  fails([&] { create_backup(f.store, root / "pre-cancelled", {}, {stop.get_token()}); });
  require(!fs::exists(root / "pre-cancelled"), "cancelled backup published a destination");
  const auto before = f.physical();
  fails([&] {
    create_backup(f.store, root / "cancelled", {}, {{}, [&](const ProgressEvent &event) {
                                                      if (event.phase == ProgressPhase::publishing)
                                                        throw Cancelled("backup publication sentinel");
                                                    }});
  });
  require(!fs::exists(root / "cancelled") && f.physical() == before,
          "backup observer failure published an incomplete destination");
  for (const auto &entry : fs::directory_iterator(root))
    require(!entry.path().filename().string().starts_with(".syrnck-export-"),
            "cancelled backup leaked a private staging directory");
  fails([&] { create_backup(f.store, f.store.root() / "nested"); });
  TransferSession session;
  session.direction = "pull";
  session.endpoint = hash(bytes("test endpoint"));
  session.root = {Kind::revision, revision};
  session.reference = "download";
  session.id =
      TransferSession::identity(session.direction, session.endpoint, session.root, session.reference);
  f.store.write_transfer_session(session);
  fails([&] { create_backup(f.store, root / "incomplete-session"); });
  require(!fs::exists(root / "incomplete-session"), "backup copied an incomplete transfer session");
}
} // namespace
int main() {
  Temporary temporary;
  try {
    for (bool encrypted : {false, true})
      for (unsigned layout = 0; layout < 3; ++layout) {
        std::optional<Key> key;
        if (encrypted) {
          key.emplace();
          key->fill(41);
        }
        snapshots(temporary.path / (std::to_string(encrypted) + '-' + std::to_string(layout)), key, layout);
      }
    repair(temporary.path / "repair");
    cancellation(temporary.path / "cancel");
    std::cout << "Verified portable store backups, key changes, restore and missing-object repair\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
