#include "native_history_fixture.hpp"
#include "pack_storage.hpp"
#include <syrnck/backup.hpp>
#include <iostream>
using namespace native_test;
namespace {
std::map<std::string, std::string> stored(const fs::path &root) {
  std::map<std::string, std::string> result;
  for (const auto &entry : fs::recursive_directory_iterator(root))
    if (entry.is_regular_file() && entry.path().filename() != "store.lock")
      result.emplace(utf8(entry.path().lexically_relative(root)), hash(read_bytes(entry.path())));
  return result;
}
fs::path loose(const Store &store, const ObjectKey &key) {
  return store.root() / "objects" / kind_name(key.kind) / key.id.substr(0, 2) / key.id;
}
void damage(Store &store, const ObjectKey &key, unsigned layout) {
  if (layout == 0) {
    atomic_write(loose(store, key), bytes("damaged original envelope"), true);
    return;
  }
  const auto config = parse_json(read_bytes(store.root() / "config.json"));
  PackStorage packs(store.root());
  packs.load(config.contains("packs") ? config["packs"] : config["pack"]);
  const auto position = packs.locate(key);
  require(position.has_value(), "repair-copy packed damage fixture missing object");
  const auto catalog = packs.catalog();
  const auto name = catalog[position->segment]["name"].get<std::string>();
  const auto path = store.root() / "packs" / ("pack-" + name + ".syp");
  const auto index = read_bytes(store.root() / "packs" / ("pack-" + name + ".syi"));
  const auto record = PackIndex::record(std::span<const std::uint8_t>(index).subspan(PackIndex::header_size),
                                        position->ordinal);
  auto envelope = read_bytes(path);
  envelope.at(static_cast<std::size_t>(record.offset)) ^= 0xff;
  atomic_write(path, envelope, true);
}
void reconstruction(const fs::path &root, std::optional<Key> source_key, unsigned layout) {
  Fixture f(root / "source", source_key);
  const auto old_file = f.file("old asset retained in backup");
  const auto manifest = old_file["manifest"].get<std::string>();
  const auto chunk = f.store.metadata(Kind::manifest, manifest)["blocks"][0]["id"].get<std::string>();
  const auto first = f.revision(Json{{"asset", old_file}});
  f.install(first);
  std::optional<Key> backup_key, output_key;
  if (source_key) {
    backup_key.emplace();
    backup_key->fill(77);
    output_key.emplace();
    output_key->fill(79);
  }
  create_backup(f.store, root / "backup", backup_key);
  Store backup(root / "backup", backup_key);
  const auto newer = f.file("newer data absent from old backup");
  const auto head = f.revision(Json{{"asset", old_file}, {"new.cpp", newer}}, Json::array({first}));
  f.install(head);
  f.store.create_native_branch("working", head);
  f.store.switch_directory(f.project, "working");
  f.write("new.cpp", "separately frozen newer work");
  const auto index = f.store.stage_directory(f.project, "working", {"new.cpp"});
  f.pack(layout);
  damage(f.store, {Kind::chunk, chunk}, layout);
  if (layout == 0)
    fs::remove(loose(f.store, {Kind::manifest, manifest}));
  else
    damage(f.store, {layout == 1 ? Kind::revision : Kind::manifest, layout == 1 ? first : manifest}, layout);
  f.store.clear_cache();
  const auto source_bytes = f.physical();
  const auto backup_bytes = stored(backup.root());
  const auto plan = plan_repaired_copy(f.store, backup);
  require(plan["ready"] == true && plan["corrupt"].size() >= 1 &&
              plan["missing"].size() == (layout == 0 ? 1 : 0) && f.physical() == source_bytes &&
              stored(backup.root()) == backup_bytes,
          "repair-copy plan wrote inputs or failed to cover current roots through old backup");
  if (source_key) {
    fails([&] { create_repaired_copy(f.store, backup, root / "downgrade"); });
    require(!fs::exists(root / "downgrade"), "repair-copy silently removed encryption");
  }
  const auto result = create_repaired_copy(f.store, backup, root / "repaired", output_key);
  require(result["state"] == "recovered" && result["objects"] == plan["objects"] &&
              result["recovered_objects"] == plan["missing"].size() + plan["corrupt"].size() &&
              f.physical() == source_bytes && stored(backup.root()) == backup_bytes &&
              !fs::exists(root / "repaired/backup.json"),
          "repair-copy changed source/donor or reported another closure");
  Store repaired(root / "repaired", output_key);
  require(repaired.refs().at("heads/main").id == head && repaired.refs().at("staging/working").id == index &&
              !repaired.refs().contains("workspace/HEAD") &&
              repaired.settings().json() == f.store.settings().json(),
          "repair-copy lost newer references/settings or transplanted workspace binding");
  repaired.gc(true);
  repaired.pack();
  repaired.fsck();
  repaired.export_revision(first, root / "original-version");
  repaired.export_revision(head, root / "newer-version");
  repaired.export_revision(index, root / "staged-version");
  require(text(read_bytes(root / "original-version/asset")) == "old asset retained in backup" &&
              text(read_bytes(root / "newer-version/new.cpp")) == "newer data absent from old backup" &&
              text(read_bytes(root / "staged-version/new.cpp")) == "separately frozen newer work",
          "repaired copy lost mixed old/new/index bytes after GC/packing");
  fails([&] { create_repaired_copy(f.store, backup, root / "repaired", output_key); });
  fails([&] { create_repaired_copy(f.store, backup, backup.root() / "nested", output_key); });
  require(f.physical() == source_bytes && stored(backup.root()) == backup_bytes,
          "failed repair-copy altered inputs");
}
void insufficient_and_cancellation(const fs::path &root) {
  Fixture f(root / "source");
  const auto asset = f.file("original asset");
  const auto first = f.revision(Json{{"asset", asset}});
  f.install(first);
  create_backup(f.store, root / "backup");
  Store backup(root / "backup");
  const auto newer = f.file("new unique file");
  const auto next = f.revision(Json{{"asset", asset}, {"new", newer}}, Json::array({first}));
  f.install(next);
  const ObjectKey absent{Kind::manifest, newer["manifest"].get<std::string>()};
  fs::remove(loose(f.store, absent));
  f.store.clear_cache();
  const auto before = f.physical();
  fails([&] { create_repaired_copy(f.store, backup, root / "incomplete"); });
  require(!fs::exists(root / "incomplete") && f.physical() == before,
          "uncovered repair-copy wrote an incomplete destination");
  require(f.file("new unique file")["manifest"] == newer["manifest"],
          "repair-copy fixture failed to restore missing source");
  const auto complete = f.physical();
  fails([&] {
    create_repaired_copy(f.store, backup, root / "cancelled", {},
                         {{}, [&](const ProgressEvent &event) {
                            if (event.phase == ProgressPhase::publishing)
                              throw Cancelled("repair-copy cancellation");
                          }});
  });
  require(!fs::exists(root / "cancelled") && f.physical() == complete,
          "cancelled repair-copy published or changed source");
  fails([&] {
    create_repaired_copy(f.store, backup, root / "changed", {},
                         {{}, [&](const ProgressEvent &event) {
                            if (event.phase == ProgressPhase::publishing)
                              f.store.set_ref("pins/concurrent", {Kind::revision, first});
                          }});
  });
  require(!fs::exists(root / "changed"), "repair-copy ignored observer reference mutation");
  f.store.remove_ref("pins/concurrent");
  fails([&] {
    create_repaired_copy(f.store, backup, root / "changed-donor", {},
                         {{}, [&](const ProgressEvent &event) {
                            if (event.phase == ProgressPhase::publishing)
                              backup.set_ref("pins/concurrent", {Kind::revision, first});
                          }});
  });
  require(!fs::exists(root / "changed-donor"), "repair-copy ignored observer donor mutation");
  backup.remove_ref("pins/concurrent");
  fails([&] {
    create_backup(f.store, root / "changed-backup", {}, {{}, [&](const ProgressEvent &event) {
                                                           if (event.phase == ProgressPhase::publishing)
                                                             f.store.set_ref("pins/concurrent",
                                                                             {Kind::revision, first});
                                                         }});
  });
  require(!fs::exists(root / "changed-backup"), "backup ignored observer reference mutation");
}
void encrypted_donor(const fs::path &root) {
  Fixture f(root / "source");
  const auto file = f.file("plaintext primary with encrypted donor");
  const auto revision = f.revision(Json{{"asset", file}});
  f.install(revision);
  Key secret;
  secret.fill(81);
  create_backup(f.store, root / "backup", secret);
  Store backup(root / "backup", secret);
  fs::remove(loose(f.store, {Kind::manifest, file["manifest"].get<std::string>()}));
  f.store.clear_cache();
  fails([&] { create_repaired_copy(f.store, backup, root / "downgraded"); });
  require(!fs::exists(root / "downgraded"),
          "repair-copy decrypted protected donor data without destination key");
  create_repaired_copy(f.store, backup, root / "protected", secret);
  Store output(root / "protected", secret);
  require(output.encrypted() && output.fsck() > 0, "encrypted donor did not produce a valid protected copy");
}
} // namespace
int main() {
  Temporary temp;
  try {
    for (bool encrypted : {false, true})
      for (unsigned layout = 0; layout < 3; ++layout) {
        std::optional<Key> key;
        if (encrypted) {
          key.emplace();
          key->fill(71);
        }
        reconstruction(temp.path / (std::to_string(encrypted) + '-' + std::to_string(layout)), key, layout);
      }
    insufficient_and_cancellation(temp.path / "failure");
    encrypted_donor(temp.path / "encrypted-donor");
    std::cout << "Verified repair copies recover corrupt loose/packed objects while preserving newer roots\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
