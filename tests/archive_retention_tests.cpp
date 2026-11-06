#include "native_history_fixture.hpp"
#include <syrnck/backup.hpp>
#include <syrnck/retention.hpp>
#include <iostream>
using namespace native_test;
namespace {
void retirement(const fs::path &root, std::optional<Key> key, unsigned layout) {
  Fixture f(root / "source", key);
  const auto old_file = f.file("old detached version"), current_file = f.file("new independent history");
  const auto old = f.revision(Json{{"asset", old_file}});
  const auto current = f.revision(Json{{"asset", current_file}});
  f.install(current);
  const auto archive = "archive/reset/" + old;
  const auto retained = "native-fetch/" + old;
  f.store.set_ref(archive, {Kind::revision, old});
  f.store.set_ref(retained, {Kind::revision, old});
  f.pack(layout);
  create_backup(f.store, root / "backup", key);
  Store backup(root / "backup", key);
  const auto original = f.physical();
  const std::vector<std::string> names{archive, retained};
  const auto plan = plan_archive_retirement(f.store, backup, names);
  require(plan["references"].size() == 2 && plan["applied"] == false && f.physical() == original,
          "archive retirement planning modified storage");
  for (const auto &invalid :
       std::vector<std::vector<std::string>>{{}, {archive, archive}, {"heads/main"}, {"unknown"}})
    fails([&] { plan_archive_retirement(f.store, backup, invalid); });
  fails([&] { retire_archives(f.store, backup, names, std::string(64, '0')); });
  require(f.physical() == original, "rejected archive retirement changed storage");
  f.store.set_ref("keep", {Kind::revision, current});
  fails([&] { retire_archives(f.store, backup, names, plan["plan"].get<std::string>()); });
  const auto fresh = plan_archive_retirement(f.store, backup, names);
  const auto before = f.physical();
  fails([&] {
    retire_archives(f.store, backup, names, fresh["plan"].get<std::string>(),
                    {{}, [&](const ProgressEvent &event) {
                       if (event.phase == ProgressPhase::publishing)
                         throw Cancelled("archive retirement sentinel");
                     }});
  });
  require(f.physical() == before, "cancelled archive retirement published references");
  const auto applied = retire_archives(f.store, backup, names, fresh["plan"].get<std::string>());
  require(applied["applied"] == true && !f.store.refs().contains(archive) &&
              !f.store.refs().contains(retained) && f.store.refs().at("heads/main").id == current,
          "archive retirement changed unrelated roots");
  f.store.gc(true);
  std::ostringstream bytes;
  backup.reconstruct(old_file["manifest"].get<std::string>(), bytes);
  require(bytes.str() == "old detached version", "archive retirement lost the durable backup version");
  require(verify_backup(backup)["state"] == "verified", "archive retirement changed its backup");
  fails([&] { retire_archives(f.store, backup, names, fresh["plan"].get<std::string>()); });
}
void coverage(const fs::path &root) {
  Fixture f(root / "source");
  const auto original = f.revision(Json{{"asset", f.file("original")}});
  f.install(original);
  create_backup(f.store, root / "backup");
  Store backup(root / "backup");
  const auto uncovered = f.revision(Json{{"asset", f.file("not backed up")}});
  const auto name = "archive/amend/" + uncovered;
  f.store.set_ref(name, {Kind::revision, uncovered});
  const auto before = f.physical();
  fails([&] { plan_archive_retirement(f.store, backup, {name}); });
  require(before == f.physical(), "uncovered retirement changed storage");
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
          key->fill(47);
        }
        retirement(temporary.path / (std::to_string(encrypted) + '-' + std::to_string(layout)), key, layout);
      }
    coverage(temporary.path / "coverage");
    std::cout << "Backup-protected archive retirement, bound plans, cancellation and retained versions\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
