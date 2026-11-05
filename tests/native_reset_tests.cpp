#include "native_history_fixture.hpp"
#include <iostream>
using namespace native_test;
namespace {
void reset_modes(const fs::path &path, std::optional<Key> key, unsigned physical) {
  Fixture f(path, key);
  const auto a = f.file("base"), b = f.file("head");
  const auto base = f.revision(Json{{"asset", a}});
  const auto head = f.revision(Json{{"asset", b}}, Json::array({base}));
  f.install(head);
  f.write("asset", "prepared");
  const auto index = f.store.stage_directory(path, "main", {"asset"});
  const auto staged_file = f.store.metadata(Kind::revision, index)["files"]["asset"];
  f.write("asset", "later working bytes");
  f.write("untracked", "keep me");
  f.store.set_ref("heads/feature", {Kind::revision, head});
  f.pack(physical);
  const auto before = f.physical();
  const auto plan = f.store.plan_reset_branch("main", base, NativeResetMode::soft, true);
  require(plan["ready"] == true && plan["changed"] == true && plan["staged"]["changes"].size() == 1 &&
              plan["index"] == index && f.physical() == before,
          "soft plan changed data or lost staging");
  fails([&] { f.store.reset_branch("main", base, base, NativeResetMode::soft); });
  std::stop_source stop;
  fails([&] {
    f.store.reset_branch("main", head, base, NativeResetMode::soft,
                         {stop.get_token(), [&](const ProgressEvent &) { stop.request_stop(); }});
  });
  require(f.physical() == before, "early reset failure wrote data");
  const auto result = f.store.reset_branch("main", head, base, NativeResetMode::soft);
  auto refs = f.store.refs();
  const auto prepared = f.store.metadata(Kind::revision, result["index"].get<std::string>());
  require(refs.at("heads/main").id == base && refs.at("heads/feature").id == head &&
              refs.at("archive/reset/" + head).id == head &&
              refs.at("archive/reset-index/" + index).id == index &&
              prepared["parents"] == Json::array({base}) && prepared["files"]["asset"] == staged_file &&
              text(read_bytes(path / "asset")) == "later working bytes" &&
              text(read_bytes(path / "untracked")) == "keep me" &&
              f.store.staged_status("main")["stale"] == false,
          "soft reset lost prepared bytes, archives or working files");
  const auto committed = f.store.commit_staged("main", "recommit prepared bytes");
  require(f.store.metadata(Kind::revision, committed)["parents"] == Json::array({base}) &&
              f.store.metadata(Kind::revision, committed)["files"]["asset"] == staged_file,
          "soft reset did not support staged recommit");
  const auto later = f.store.stage_directory(path, "main", {"asset"});
  f.store.set_ref("heads/main", {Kind::revision, head}); // Deliberately stale staging.
  require(f.store.plan_reset_branch("main", base, NativeResetMode::soft)["ready"] == false &&
              f.store.plan_reset_branch("main", base, NativeResetMode::mixed)["ready"] == true,
          "reset stale index policy differs");
  const auto mixed = f.store.reset_branch("main", head, base, NativeResetMode::mixed);
  refs = f.store.refs();
  require(mixed["index"].is_null() && !refs.contains("staging/main") &&
              refs.at("archive/reset-index/" + later).id == later &&
              text(read_bytes(path / "asset")) == "later working bytes",
          "mixed reset lost the old index or changed working bytes");
  const auto unchanged = f.physical();
  require(f.store.reset_branch("main", base, base, NativeResetMode::mixed)["state"] == "unchanged" &&
              f.physical() == unchanged,
          "unchanged reset wrote data");
  f.store.gc(true);
  for (const auto &id : {head, index, later})
    (void)f.store.closure({{Kind::revision, id}}, true);
  require(f.store.fsck() > 0, "reset damaged retained storage");
}
void implicit_index_and_guards(const fs::path &path) {
  Fixture f(path);
  const auto root = f.revision(Json{{"asset", f.file("root")}});
  const auto head = f.revision(Json{{"asset", f.file("head")}}, Json::array({root}));
  f.install(head);
  const auto no_op = f.physical();
  f.store.reset_branch("main", head, head, NativeResetMode::soft);
  require(f.physical() == no_op, "same-head soft reset materialized an index");
  const auto saved_head = f.store.native_head();
  f.store.reset_branch("main", head, root, NativeResetMode::soft);
  require(f.store.staged_status("main")["clean"] == false &&
              f.store.native_head()["branch"] == saved_head["branch"],
          "implicit index was not frozen or HEAD selection moved");
  f.store.reset_stage("main");
  f.store.set_ref("heads/main", {Kind::revision, head});
  f.store.set_ref("archive/reset/" + head, {Kind::revision, root});
  const auto before = f.physical();
  require(f.store.plan_reset_branch("main", root, NativeResetMode::mixed)["blockers"][0]["reason"] ==
              "archive_conflict",
          "reset adopted an unrelated archive");
  fails([&] { f.store.reset_branch("main", head, root, NativeResetMode::mixed); });
  fails([&] { f.store.plan_reset_branch("main", root, static_cast<NativeResetMode>(99)); });
  require(f.physical() == before, "blocked reset wrote data");
  f.store.remove_ref("archive/reset/" + head);
  f.install(head);
  std::stop_source stop;
  try {
    f.store.checkout_directory(path, "main", root,
                               {stop.get_token(), [&](const ProgressEvent &event) {
                                  if (event.phase == ProgressPhase::exporting && event.bytes)
                                    stop.request_stop();
                                }});
  } catch (const Cancelled &) {
  }
  require(f.store.checkout_status()["pending"] == true &&
              f.store.plan_reset_branch("main", root, NativeResetMode::mixed)["ready"] == false,
          "pending checkout did not block reset");
  fails([&] { f.store.reset_branch("main", head, root, NativeResetMode::mixed); });
  f.store.recover_checkout(true);
}
void missing_old_payload(const fs::path &path) {
  std::string root, head, chunk;
  Bytes envelope;
  fs::path block;
  {
    Fixture f(path);
    root = f.revision(Json{{"asset", f.file("base")}});
    const auto file = f.file("old exclusive payload");
    head = f.revision(Json{{"asset", file}}, Json::array({root}));
    f.install(head);
    const auto value = f.store.metadata(Kind::manifest, file["manifest"].get<std::string>());
    chunk = value["blocks"][0]["id"].get<std::string>();
    block = path / ".syrnck/objects/chunk" / chunk.substr(0, 2) / chunk;
    envelope = read_bytes(block);
  }
  require(fs::remove(block), "missing reset fixture failed");
  Store store(path / ".syrnck");
  const auto before = store.refs();
  require(store.plan_reset_branch("main", root, NativeResetMode::mixed)["ready"] == true,
          "metadata reset plan read block payloads");
  fails([&] { store.reset_branch("main", head, root, NativeResetMode::mixed); });
  require(store.refs() == before, "reset dropped a damaged old history");
  atomic_write(block, envelope, false);
  store.reset_branch("main", head, root, NativeResetMode::mixed);
}
} // namespace
int main() {
  Temporary temp;
  try {
    for (bool encrypted : {false, true})
      for (unsigned physical = 0; physical != 3; ++physical) {
        std::optional<Key> key;
        if (encrypted) {
          key.emplace();
          key->fill(47);
        }
        reset_modes(temp.path / (std::to_string(encrypted) + '-' + std::to_string(physical)), key, physical);
      }
    implicit_index_and_guards(temp.path / "guards");
    missing_old_payload(temp.path / "missing");
    std::cout << "soft/mixed reset, retained indexes, dirty workspace, guards, cancellation and GC passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
