#include "native_history_fixture.hpp"
#include <syrnck/backup.hpp>
#include <iostream>
using namespace native_test;
namespace {
const std::vector<std::string> linked{"linked-workspaces-v1"};
void scenario(const fs::path &root, std::optional<Key> key, unsigned layout) {
  std::string revision;
  {
    Fixture f(root / "source", key);
    revision = f.revision(Json{{"asset", f.file("retained feature-gated data")}});
    f.install(revision);
    f.pack(layout);
    const auto path = f.store.root() / "config.json";
    const auto original = read_bytes(path);
    const auto refs = f.store.refs();
    require(f.store.required_features().empty(), "legacy Store unexpectedly requires features");
    f.store.require_features({});
    fails([&] { f.store.require_features({"future-unknown-feature"}); });
    require(read_bytes(path) == original, "unknown feature changed Store configuration");
    std::stop_source before;
    before.request_stop();
    fails([&] { f.store.require_features(linked, {before.get_token()}); });
    std::stop_source publishing;
    fails([&] { f.store.require_features(linked, {publishing.get_token(), [&](const ProgressEvent &event) {
      if (event.phase == ProgressPhase::publishing && event.path == "store-features") publishing.request_stop();
    }}); });
    require(read_bytes(path) == original && f.store.required_features().empty(),
            "cancelled feature publication changed configuration");
    const auto altered = text(original) + '\n';
    fails([&] { f.store.require_features(linked, {{}, [&](const ProgressEvent &event) {
      if (event.phase == ProgressPhase::publishing) atomic_write(path, bytes(altered), true);
    }}); });
    require(text(read_bytes(path)) == altered, "feature publication overwrote a changed configuration");
    atomic_write(path, original, true);
    f.store.require_features(linked);
    const auto enabled = read_bytes(path);
    const auto document = parse_json(enabled);
    require(document["required_features"] == Json(linked) &&
                document["version"] == parse_json(original)["version"] && f.store.refs() == refs,
            "feature requirement changed physical format, roots or feature identity");
    f.store.require_features({linked.front(), linked.front()});
    require(read_bytes(path) == enabled, "requiring an enabled feature rewrote configuration");
    f.file("new loose data after feature publication");
    f.pack(layout);
    f.store.gc(true);
    require(parse_json(read_bytes(path))["required_features"] == Json(linked), "maintenance removed required features");
    create_backup(f.store, root / "backup", key);
    Store backup(root / "backup", key);
    require(backup.required_features() == linked, "backup dropped reader requirements");
    const auto descriptor_path = backup.root() / "backup.json";
    const auto descriptor = read_bytes(descriptor_path);
    auto marker = parse_json(descriptor);
    require(marker["version"] == 3 && marker.size() == 10 && marker["required_features"] == Json(linked),
            "feature-gated backup did not bind required features in marker v3");
    verify_backup(backup);
    marker["required_features"] = Json::array();
    atomic_write(descriptor_path, bytes(marker.dump()), true);
    fails([&] { verify_backup(backup); });
    atomic_write(descriptor_path, descriptor, true);
    restore_backup(backup, root / "restored", key);
    Store restored(root / "restored", key);
    require(restored.required_features() == linked && restored.fsck() > 0, "restore dropped required features");
    create_repaired_copy(f.store, backup, root / "repaired", key);
    Store repaired(root / "repaired", key);
    require(repaired.required_features() == linked && repaired.fsck() > 0, "repair copy dropped required features");
  }
  Store reopened(root / "source/.syrnck", key);
  require(reopened.required_features() == linked, "reopening forgot required features");
  reopened.export_revision(revision, root / "export");
  require(text(read_bytes(root / "export/asset")) == "retained feature-gated data", "feature-gated Store lost file bytes");
}
void invalid_configurations(const fs::path &root, std::optional<Key> key) {
  Json original;
  {
    Store store(root, key, true);
    original = parse_json(read_bytes(root / "config.json"));
  }
  const std::vector<Json> invalid{Json::array(), Json::array({"future-unknown-feature"}),
      Json::array({"linked-workspaces-v1", "linked-workspaces-v1"}), Json::array({false}),
      Json("linked-workspaces-v1"), Json{}, Json::array({"linked-workspaces-v1", "future-unknown-feature"})};
  for (const auto &features : invalid) {
    auto value = original;
    value["required_features"] = features;
    const auto data = value.dump();
    atomic_write(root / "config.json", bytes(data), true);
    fails([&] { Store store(root, key); });
    require(text(read_bytes(root / "config.json")) == data, "rejected feature configuration was overwritten");
  }
  original["required_features"] = linked;
  atomic_write(root / "config.json", bytes(original.dump()), true);
  Store supported(root, key);
  require(supported.required_features() == linked, "supported extended configuration was refused");
}
void automatic_gate(const fs::path &root) {
  {
    Fixture f(root / "source");
    const auto revision = f.revision(Json{{"asset", f.file("shared data")}});
    f.install(revision);
    f.store.set_ref("workspaces/user/nested/registration", {Kind::revision, revision});
    require(f.store.required_features().empty(), "an unrelated nested reference enabled workspace semantics");
    f.store.register_native_workspace(f.project, "edit", root / "edit");
    require(f.store.required_features() == linked, "linked registration was published without a reader gate");
    // Simulate an older experimental Store that predates capability gating.
    auto config = parse_json(read_bytes(f.store.root() / "config.json"));
    config.erase("required_features");
    atomic_write(f.store.root() / "config.json", bytes(config.dump()), true);
  }
  Store store(root / "source/.syrnck");
  require(store.required_features().empty(), "legacy linked fixture unexpectedly gated itself on read");
  store.set_ref("pins/upgrade", store.refs().at("heads/main"));
  require(store.required_features() == linked, "legacy linked ref publication did not establish its reader gate");
}
void changing_source(const fs::path &root, std::optional<Key> key, bool repair) {
  Fixture f(root / "source", key);
  f.install(f.revision(Json{{"asset", f.file("stable retained bytes")}}));
  create_backup(f.store, root / "backup", key);
  Store backup(root / "backup", key);
  const auto destination = fs::canonical(root) / "changed-source";
  bool changed = false;
  OperationOptions options{{}, [&](const ProgressEvent &event) {
    if (event.phase == ProgressPhase::publishing && event.path == utf8(destination)) {
      changed = true;
      f.store.require_features(linked);
    }
  }};
  fails([&] {
    if (repair) create_repaired_copy(f.store, backup, destination, key, options);
    else create_backup(f.store, destination, key, options);
  });
  require(changed && !fs::exists(destination) && f.store.required_features() == linked,
          "copy published an obsolete reader contract after source requirements changed");
}
} // namespace
int main() {
  Temporary temporary;
  try {
    for (bool encrypted : {false, true}) {
      std::optional<Key> key;
      if (encrypted) { key.emplace(); key->fill(73); }
      for (unsigned layout = 0; layout < 3; ++layout)
        scenario(temporary.path / (std::to_string(encrypted) + '-' + std::to_string(layout)), key, layout);
      invalid_configurations(temporary.path / ("invalid-" + std::to_string(encrypted)), key);
      for (bool repair : {false, true})
        changing_source(temporary.path / ("change-" + std::to_string(encrypted) + '-' + std::to_string(repair)), key, repair);
    }
    automatic_gate(temporary.path / "automatic");
    std::cout << "Required reader features survive packs, GC, backup, restore and repaired copies; unknown features refuse open\n";
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
