#include "native_history_fixture.hpp"
#include "pack_storage.hpp"
#include <syrnck/backup.hpp>
#include <iostream>
using namespace native_test;
namespace {
fs::path loose(const Store &store, const ObjectKey &key) {
  return store.root() / "objects" / kind_name(key.kind) / key.id.substr(0, 2) / key.id;
}
struct PackedFile { fs::path data, index; std::uint64_t offset; std::uint32_t length; };
PackedFile packed_file(const Store &store, const ObjectKey &key) {
  const auto config = parse_json(read_bytes(store.root() / "config.json"));
  PackStorage packs(store.root()); packs.load(config.contains("packs") ? config["packs"] : config["pack"]);
  const auto position = packs.locate(key); require(position.has_value(), "fixture object not packed");
  const auto name = packs.catalog()[position->segment]["name"].get<std::string>();
  const auto index = store.root() / "packs" / ("pack-" + name + ".syi");
  const auto bytes = read_bytes(index);
  const auto record = PackIndex::record(std::span(bytes).subspan(PackIndex::header_size), position->ordinal);
  return {store.root() / "packs" / ("pack-" + name + ".syp"), index, record.offset, record.length};
}
std::string relative_name(const fs::path &root, const fs::path &path) {
  auto name = utf8(path.lexically_relative(root));
  std::replace(name.begin(), name.end(), '\\', '/');
  return name;
}
void corrupt(const Store &store, const ObjectKey &key, unsigned layout) {
  if (!layout) { atomic_write(loose(store, key), bytes("bad envelope"), true); return; }
  const auto location = packed_file(store, key);
  auto data = read_bytes(location.data); data.at(static_cast<std::size_t>(location.offset)) ^= 127;
  atomic_write(location.data, data, true);
}
std::map<std::string, std::string> physical(const fs::path &root) {
  std::map<std::string, std::string> result;
  for (const auto &entry : fs::recursive_directory_iterator(root))
    if (entry.is_regular_file() && entry.path().filename() != "store.lock")
      result.emplace(relative_name(root, entry.path()), hash(read_bytes(entry.path())));
  return result;
}
void recovered_history(const fs::path &root, std::optional<Key> key, unsigned layout) {
  Fixture f(root / "source", key);
  const auto old_file = f.file("old asset bytes retained after rewriting history");
  const auto manifest = old_file["manifest"].get<std::string>();
  const ObjectKey block{Kind::chunk, f.store.metadata(Kind::manifest, manifest)["blocks"][0]["id"].get<std::string>()};
  const auto first = f.revision(Json{{"asset", old_file}}); f.install(first);
  std::optional<Key> donor_key;
  if (key) { donor_key.emplace(); donor_key->fill(99); }
  create_backup(f.store, root / "backup", donor_key);
  Store backup(root / "backup", donor_key);
  const auto newer_file = f.file("new source absent from the older backup");
  // Rewrite away the old parent/file: only the archive now retains its bytes.
  f.store.set_ref("archive/amend/" + first, {Kind::revision, first});
  const auto head = f.revision(Json{{"new.cpp", newer_file}}); f.install(head);
  f.store.create_native_branch("prepared", head); f.store.switch_directory(f.project, "prepared");
  f.write("new.cpp", "frozen staging bytes");
  const auto index = f.store.stage_directory(f.project, "prepared", {"new.cpp"});
  f.pack(layout);
  const auto config_bytes = read_bytes(f.store.root() / "config.json");
  const auto refs = f.store.refs(); const auto refs_bytes = read_bytes(f.store.root() / "refs.json");
  const auto settings = f.store.settings().json(); const auto donor_before = physical(backup.root());
  const auto old_pack = layout ? std::optional(packed_file(f.store, block)) : std::nullopt;
  const auto inactive = f.store.root() / "packs" / ("pack-" + std::string(32, '0'));
  atomic_write(fs::path(inactive.native() + fs::path(".syp").native()), bytes("unrelated inactive pack"), false);
  atomic_write(fs::path(inactive.native() + fs::path(".syi").native()), bytes("unrelated inactive index"), false);
  corrupt(f.store, block, layout);
  if (!layout) fs::remove(loose(f.store, {Kind::manifest, manifest}));
  const auto before = physical(f.store.root());
  const auto plan = plan_storage_repair(f.store, backup);
  require(plan["ready"] == true && plan["corrupt_count"] == 1 && plan["missing_count"] == (layout ? 0 : 1) &&
      plan["packs_to_rewrite"] == (layout ? 1 : 0) && physical(f.store.root()) == before && physical(backup.root()) == donor_before,
      "repair plan changed inputs or missed corruption");
  const auto repaired = repair_storage(f.store, backup, plan["plan"].get<std::string>());
  require(repaired["applied"] == true && repaired["rewritten_packs"] == (layout ? 1 : 0) &&
      f.store.refs() == refs && read_bytes(f.store.root() / "refs.json") == refs_bytes && f.store.settings().json() == settings &&
      physical(backup.root()) == donor_before, "repair changed references/settings or donor");
  const auto after = physical(f.store.root());
  if (old_pack) {
    require(!fs::exists(old_pack->data) && !fs::exists(old_pack->index), "old corrupt pack remained active");
    for (const auto &[path, digest] : before)
      if (path.starts_with("packs/") && path != relative_name(f.store.root(), old_pack->data) &&
          path != relative_name(f.store.root(), old_pack->index))
        require(after.at(path) == digest, "repair rewrote an unaffected pack");
  } else require(read_bytes(f.store.root() / "config.json") == config_bytes, "loose repair changed config");
  const auto clean = plan_storage_repair(f.store, backup);
  require(clean["corrupt_count"] == 0 && clean["missing_count"] == 0, "repair left a damaged copy");
  repair_storage(f.store, backup, clean["plan"].get<std::string>());
  require(physical(f.store.root()) == after, "clean repair was not idempotent");
  f.store.gc(true); f.store.fsck();
  f.store.export_revision(first, root / "old"); f.store.export_revision(head, root / "new"); f.store.export_revision(index, root / "index");
  require(text(read_bytes(root / "old/asset")) == "old asset bytes retained after rewriting history" &&
      text(read_bytes(root / "new/new.cpp")) == "new source absent from the older backup" &&
      text(read_bytes(root / "index/new.cpp")) == "frozen staging bytes", "repair lost a retained version");
}
void refuse_stale_and_unrecoverable(const fs::path &root) {
  Fixture f(root / "source");
  const auto file = f.file("retained"); const auto revision = f.revision(Json{{"file", file}}); f.install(revision);
  create_backup(f.store, root / "backup"); Store backup(root / "backup");
  const auto manifest = file["manifest"].get<std::string>();
  const ObjectKey chunk{Kind::chunk, f.store.metadata(Kind::manifest, manifest)["blocks"][0]["id"].get<std::string>()};
  corrupt(f.store, chunk, 0);
  auto plan = plan_storage_repair(f.store, backup);
  const auto unrelated = f.store.put(Kind::chunk, bytes("new unreferenced object")).first;
  const auto before = physical(f.store.root());
  fails([&] { repair_storage(f.store, backup, plan["plan"].get<std::string>()); });
  require(physical(f.store.root()) == before, "stale repair wrote bytes");
  plan = plan_storage_repair(f.store, backup);
  std::stop_source stopped; stopped.request_stop();
  fails([&] { repair_storage(f.store, backup, plan["plan"].get<std::string>(), {stopped.get_token()}); });
  require(physical(f.store.root()) == before, "pre-cancelled repair wrote bytes");
  corrupt(f.store, unrelated, 0);
  const auto blocked = physical(f.store.root());
  fails([&] { plan_storage_repair(f.store, backup); });
  require(physical(f.store.root()) == blocked, "unrecoverable unreferenced data was discarded");
}
void shadowed_copies(const fs::path &root, std::optional<Key> secret) {
  Fixture f(root / "source", secret);
  const auto old = f.file("backup original"); f.install(f.revision(Json{{"old", old}}));
  create_backup(f.store, root / "backup", secret); Store backup(root / "backup", secret);
  const auto a = f.file("new A absent from backup"), b = f.file("new B absent from backup");
  f.install(f.revision(Json{{"old", old}, {"a", a}, {"b", b}})); f.pack(2);
  auto block = [&](const Json &entry) {
    return ObjectKey{Kind::chunk, f.store.metadata(Kind::manifest, entry["manifest"].get<std::string>())["blocks"][0]["id"].get<std::string>()};
  };
  const auto first = block(a), second = block(b);
  require(!backup.contains(first) && !backup.contains(second), "shadow fixture unexpectedly has newer backup bytes");
  for (const auto &key : {first, second}) {
    const auto item = packed_file(f.store, key); const auto data = read_bytes(item.data);
    atomic_write(loose(f.store, key), std::span(data).subspan(static_cast<std::size_t>(item.offset), item.length), false);
  }
  corrupt(f.store, first, 0); corrupt(f.store, second, 2);
  const auto plan = plan_storage_repair(f.store, backup);
  require(plan["corrupt_count"] == 2 && plan["corrupt_loose_copies"] == 1 && plan["corrupt_packed_copies"] == 1,
          "audit hid a damaged secondary copy");
  repair_storage(f.store, backup, plan["plan"].get<std::string>());
  require(text(f.store.get(first)) == "new A absent from backup" && text(f.store.get(second)) == "new B absent from backup",
          "repair did not use the healthy local alternate copy");
  f.store.fsck();
}
void interrupted_and_guarded(const fs::path &root) {
  Fixture f(root / "source");
  const auto a = f.file("a"), b = f.file("b"); f.install(f.revision(Json{{"a", a}, {"b", b}}));
  create_backup(f.store, root / "backup"); Store backup(root / "backup");
  std::vector<ObjectKey> chunks;
  for (const auto &entry : {a, b}) chunks.push_back({Kind::chunk,
      f.store.metadata(Kind::manifest, entry["manifest"].get<std::string>())["blocks"][0]["id"].get<std::string>()});
  std::sort(chunks.begin(), chunks.end());
  for (const auto &chunk : chunks) corrupt(f.store, chunk, 0);
  auto plan = plan_storage_repair(f.store, backup); const auto refs = f.store.refs();
  std::stop_source stop; unsigned mutations = 0;
  fails([&] { repair_storage(f.store, backup, plan["plan"].get<std::string>(), {stop.get_token(), [&](const ProgressEvent &event) {
    if (event.phase == ProgressPhase::transferring && ++mutations == 2) stop.request_stop();
  }}); });
  require(f.store.refs() == refs && text(f.store.get(chunks[0])).size() == 1, "interruption discarded completed repair or roots");
  fails([&] { f.store.get(chunks[1]); });
  plan = plan_storage_repair(f.store, backup);
  require(plan["corrupt_count"] == 1, "fresh plan did not resume partial progress");
  const auto raw = read_bytes(loose(f.store, chunks[1]));
  const auto extra = f.store.put(Kind::chunk, bytes("callback retained object")).first;
  plan = plan_storage_repair(f.store, backup);
  fails([&] { repair_storage(f.store, backup, plan["plan"].get<std::string>(), {{}, [&](const ProgressEvent &event) {
    if (event.phase == ProgressPhase::transferring) f.store.set_ref("callback", extra);
  }}); });
  require(read_bytes(loose(f.store, chunks[1])) == raw && f.store.refs().contains("callback"),
          "repair ignored a publication callback changing roots");
  plan = plan_storage_repair(f.store, backup); repair_storage(f.store, backup, plan["plan"].get<std::string>()); f.store.fsck();
}
void oversized_and_donor_guard(const fs::path &root) {
  Fixture f(root / "source"); const auto file = f.file("recover oversized corruption");
  f.install(f.revision(Json{{"asset", file}}));
  create_backup(f.store, root / "backup"); Store backup(root / "backup");
  const ObjectKey chunk{Kind::chunk, f.store.metadata(Kind::manifest, file["manifest"].get<std::string>())["blocks"][0]["id"].get<std::string>()};
  fs::resize_file(loose(f.store, chunk), max_object_size + 1024 * 1024 + 1);
  bool changed = false;
  fails([&] { plan_storage_repair(f.store, backup, {{}, [&](const ProgressEvent &) {
    if (!changed) { changed = true; backup.set_ref("unexpected", chunk); }
  }}); });
  require(changed && fs::file_size(loose(f.store, chunk)) > max_object_size, "changed donor was accepted or source was written");
  backup.remove_ref("unexpected");
  const auto plan = plan_storage_repair(f.store, backup);
  require(plan["corrupt_count"] == 1, "oversized envelope was not classified as corrupt");
  repair_storage(f.store, backup, plan["plan"].get<std::string>());
  require(text(f.store.get(chunk)) == "recover oversized corruption", "oversized envelope recovery changed bytes");
  create_backup(f.store, root / "second-backup"); Store second_backup(root / "second-backup");
  corrupt(backup, chunk, 0);
  const auto immutable_before = physical(backup.root()), donor_before = physical(second_backup.root());
  fails([&] { plan_storage_repair(backup, second_backup); });
  require(physical(backup.root()) == immutable_before && physical(second_backup.root()) == donor_before,
          "storage repair modified an immutable backup target or its donor");
}
}
int main() {
  Temporary temporary;
  try {
    for (unsigned encrypted = 0; encrypted < 2; ++encrypted) {
      std::optional<Key> key; if (encrypted) { key.emplace(); key->fill(57); }
      for (unsigned layout = 0; layout < 3; ++layout)
        recovered_history(temporary.path / (std::to_string(encrypted) + "-" + std::to_string(layout)), key, layout);
      shadowed_copies(temporary.path / ("shadow-" + std::to_string(encrypted)), key);
    }
    refuse_stale_and_unrecoverable(temporary.path / "guards");
    interrupted_and_guarded(temporary.path / "interrupted");
    oversized_and_donor_guard(temporary.path / "oversized");
    std::cout << "Verified in-place storage repair across plain/encrypted loose/v2/v3 histories\n";
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
