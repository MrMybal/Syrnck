#include "native_history_fixture.hpp"
#include <syrnck/checkout_retention.hpp>
#include <syrnck/workspace.hpp>
#include <syrnck/backup.hpp>
#include <iostream>
using namespace native_test;
namespace {
void workflow(const fs::path &root, std::optional<Key> key, unsigned layout, bool sources) {
  Fixture f(root / "project", key);
  const auto old = f.revision(Json{{"asset", f.file("old version")}});
  const auto target = f.revision(Json{{"asset", f.file("new independent version")}});
  f.install(old);
  f.write("asset", "separately staged");
  const auto index = f.store.stage_directory(f.project, "main", {"asset"});
  f.write("asset", "dirty working version");
  const auto reset_plan = plan_native_hard_reset(f.store, f.project, "main", target);
  const auto reset =
      native_hard_reset(f.store, f.project, "main", old, target, reset_plan["plan"].get<std::string>());
  const auto id = reset["id"].get<std::string>();
  const auto source = reset["source"].get<std::string>();
  const std::string receipt = "checkouts/receipts/" + id, completed = "checkouts/completed/" + id,
                    archive = "archive/checkout/" + id;
  f.pack(layout);
  create_backup(f.store, root / "backup", key);
  Store backup(root / "backup", key);
  const auto before = f.physical();
  const auto list = native_checkout_receipts(f.store, {}, 1, true);
  require(list["transactions"].size() == 1 && list["transactions"][0]["id"] == id &&
              list["transactions"][0]["retained_only"] == false && list["has_more"] == false &&
              native_checkout_receipts(f.store, id)["transactions"].empty() && f.physical() == before,
          "checkout receipt inventory wrote files or lost cursor");
  const auto plan = plan_checkout_retirement(f.store, backup, {id}, sources);
  require(plan["references"].size() == (sources ? 3 : 2) && plan["applied"] == false &&
              f.physical() == before,
          "checkout retirement planning wrote files or lost reference group");
  for (const auto &ids : std::vector<std::vector<std::string>>{{}, {id, id}, {std::string(64, 'a')}, {"bad"}})
    fails([&] { plan_checkout_retirement(f.store, backup, ids, sources); });
  fails([&] { native_checkout_receipts(f.store, {}, 0); });
  fails([&] { plan_checkout_retirement(f.store, f.store, {id}, sources); });
  fails([&] { retire_checkouts(f.store, backup, {id}, std::string(64, '0'), sources); });
  fails([&] {
    retire_checkouts(f.store, backup, {id}, plan["plan"].get<std::string>(), sources,
                     {{}, [&](const ProgressEvent &) { throw Cancelled("retirement cancelled"); }});
  });
  require(f.physical() == before, "rejected checkout retirement changed files");
  const auto marker = read_bytes(backup.root() / "backup.json");
  fails([&] {
    retire_checkouts(f.store, backup, {id}, plan["plan"].get<std::string>(), sources,
                     {{}, [&](const ProgressEvent &) {
                        auto changed = marker;
                        changed.push_back(' ');
                        atomic_write(backup.root() / "backup.json", changed, true);
                      }});
  });
  atomic_write(backup.root() / "backup.json", marker, true);
  require(f.physical() == before, "changed backup observer retired checkout roots");
  fails([&] {
    retire_checkouts(
        f.store, backup, {id}, plan["plan"].get<std::string>(), sources,
        {{}, [&](const ProgressEvent &) { f.store.set_ref("observer/pin", {Kind::revision, target}); }});
  });
  require(f.store.refs().contains(receipt) && f.store.refs().contains(completed) &&
              f.store.refs().contains("observer/pin"),
          "changed source observer lost its update or retired checkout roots");
  f.store.remove_ref("observer/pin");
  const auto foreign = f.store.root() / "checkouts" / id;
  fs::create_directories(foreign);
  atomic_write(foreign / "foreign", bytes("preserve"), false);
  fails([&] { plan_checkout_retirement(f.store, backup, {id}, sources); });
  require(text(read_bytes(foreign / "foreign")) == "preserve", "retirement deleted private data");
  // Model the operator preserving unexpected files outside the private area.
  // Keeping the file also avoids a Windows delete-pending directory race.
  fs::rename(foreign, root / "preserved-private-data");
  require(text(read_bytes(root / "preserved-private-data/foreign")) == "preserve",
          "fixture lost preserved private bytes");
  restore_backup(backup, root / "restored", key);
  Store restored(root / "restored", key);
  require(native_checkout_receipts(restored, {}, 100, true)["transactions"][0]["retained_only"] == true,
          "restored backup invented a workspace receipt");
  const auto restored_plan = plan_checkout_retirement(restored, backup, {id}, sources);
  retire_checkouts(restored, backup, {id}, restored_plan["plan"].get<std::string>(), sources);
  require(native_checkout_receipts(restored)["transactions"].empty(),
          "restored target root could not be retired");
  const auto applied = retire_checkouts(f.store, backup, {id}, plan["plan"].get<std::string>(), sources);
  require(applied["applied"] == true && !f.store.refs().contains(receipt) &&
              !f.store.refs().contains(completed) && f.store.refs().contains(archive) != sources &&
              f.store.refs().at("heads/main").id == target &&
              text(read_bytes(f.project / "asset")) == "new independent version",
          "checkout retirement split group, touched workspace or moved head");
  f.store.gc(true);
  std::ostringstream output;
  const auto files = backup.metadata(Kind::revision, source)["files"];
  backup.reconstruct(files["asset"]["manifest"].get<std::string>(), output);
  require(output.str() == "dirty working version", "retirement lost backup working bytes");
  output.str("");
  output.clear();
  backup.reconstruct(backup.metadata(Kind::revision, index)["files"]["asset"]["manifest"].get<std::string>(),
                     output);
  require(output.str() == "separately staged" && verify_backup(backup)["state"] == "verified",
          "retirement lost original index or changed backup");
  fails([&] { f.store.checkout_status(id); });
  require(native_checkout_receipts(f.store)["transactions"].empty(), "retired receipt remains listed");
}
void historical_receipt(const fs::path &root) {
  Fixture f(root);
  const auto head = f.revision(Json{{"asset", f.file("base")}});
  f.install(head);
  f.store.create_native_branch("other", head);
  f.store.switch_directory(root, "other");
  const auto old_selection = f.store.refs().at("workspace/HEAD");
  f.write("asset", "dirty");
  const auto planned = plan_native_hard_reset(f.store, root, "other", head);
  const auto applied =
      native_hard_reset(f.store, root, "other", head, head, planned["plan"].get<std::string>());
  const auto id = applied["id"].get<std::string>();
  const auto source = applied["source"].get<std::string>();
  f.store.switch_directory(root, "main");
  f.store.remove_ref("archive/checkout/" + id);
  f.store.gc(true);
  require(!f.store.contains(old_selection) && !f.store.contains({Kind::revision, source}),
          "historical receipt fixture still retains obsolete selection/source");
  const auto before = f.physical();
  const auto receipt = f.store.checkout_status(id);
  require(receipt["pending"] == false && receipt["source"] == source && receipt["state"] == "committed" &&
              receipt["files"].is_null() && receipt["files_available"] == false && f.physical() == before,
          "historical receipt required collected HEAD/source or wrote files");
  require(native_checkout_receipts(f.store, {}, 100, true)["transactions"].size() == 3,
          "historical receipt inventory lost a completed operation after GC");
}
void coverage(const fs::path &root) {
  Fixture f(root / "project");
  const auto head = f.revision(Json{{"asset", f.file("original")}});
  f.install(head);
  create_backup(f.store, root / "backup");
  Store backup(root / "backup");
  const auto target = f.revision(Json{{"asset", f.file("not backed up")}});
  const auto result = f.store.checkout_directory(f.project, "main", target);
  const auto before = f.physical();
  fails([&] { plan_checkout_retirement(f.store, backup, {result["id"].get<std::string>()}); });
  require(f.physical() == before, "uncovered checkout retirement wrote files");
}
} // namespace
int main() {
  Temporary temporary;
  try {
    for (bool encrypted : {false, true})
      for (unsigned layout = 0; layout < 3; ++layout)
        for (bool sources : {false, true}) {
          std::optional<Key> key;
          if (encrypted) {
            key.emplace();
            key->fill(78);
          }
          workflow(temporary.path / (std::to_string(encrypted) + '-' + std::to_string(layout) + '-' +
                                     std::to_string(sources)),
                   key, layout, sources);
        }
    historical_receipt(temporary.path / "historical");
    coverage(temporary.path / "coverage");
    std::cout << "Checkout retirement tests passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
