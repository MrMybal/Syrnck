#include "structured_archive_fixture.hpp"
#include <iostream>
using namespace structured_archive_test;
namespace {
void retirement(const fs::path &root, bool encrypted, unsigned layout) {
  const auto key = encryption_key(encrypted, 47), backup_key = encryption_key(encrypted, 59);
  Fixture f(root / "source", key);
  const auto a = populate(f);
  f.pack(layout);
  create_backup(f.store, root / "backup", backup_key);
  Store backup(root / "backup", backup_key);
  const auto backup_refs = backup.refs();
  const auto descriptor = read_bytes(backup.root() / "backup.json");
  reuse_names(f.store, a);
  const auto original = f.physical();
  const auto refs = f.store.refs();
  const auto head = f.store.native_head();
  const auto plan = plan_archive_retirement(f.store, backup, a.selectors);
  require(plan["references"].size() == 5 && plan["removed_references"] == 11 && plan["applied"] == false &&
              f.physical() == original,
          "structured retirement preview changed storage or lost group roots");
  unsigned groups = 0;
  for (const auto &row : plan["references"])
    if (row.contains("roots")) {
      require(row["kind"] == "branch-group" || row["kind"] == "tag-group", "unknown retirement kind");
      groups += static_cast<unsigned>(row["roots"].size());
    }
  require(groups == 10, "retirement preview omitted structured members");
  for (const auto &[name, value] : refs) {
    (void)value;
    if (name.starts_with("archive/branches/") || name.starts_with("archive/tags/"))
      fails([&] { plan_archive_retirement(f.store, backup, {name}); });
  }
  for (const auto &invalid :
       std::vector<std::vector<std::string>>{{},
                                             {a.selectors[0], a.selectors[0]},
                                             {"archive/branches/invalid"},
                                             {"archive/tags/" + std::string(64, '0')},
                                             {"tags/release/v1"},
                                             std::vector<std::string>(1025, a.selectors[0])})
    fails([&] { plan_archive_retirement(f.store, backup, invalid); });
  fails([&] { retire_archives(f.store, backup, a.selectors, std::string(64, '0')); });
  require(f.physical() == original, "rejected group retirement wrote data");
  f.store.set_ref("observer/pin", {Kind::revision, a.current});
  fails([&] { retire_archives(f.store, backup, a.selectors, plan["plan"].get<std::string>()); });
  f.store.remove_ref("observer/pin");
  std::stop_source stop;
  fails([&] {
    retire_archives(f.store, backup, a.selectors, plan["plan"].get<std::string>(),
                    {stop.get_token(), [&](const ProgressEvent &) { stop.request_stop(); }});
  });
  fails([&] {
    retire_archives(f.store, backup, a.selectors, plan["plan"].get<std::string>(),
                    {{}, [&](const ProgressEvent &) {
                       auto changed = descriptor;
                       changed.push_back(' ');
                       atomic_write(backup.root() / "backup.json", changed, true);
                     }});
  });
  atomic_write(backup.root() / "backup.json", descriptor, true);
  require(f.physical() == original, "cancelled or uncovered group retirement wrote data");
  fails([&] {
    retire_archives(
        f.store, backup, a.selectors, plan["plan"].get<std::string>(),
        {{}, [&](const ProgressEvent &) { f.store.set_ref("observer/pin", {Kind::revision, a.current}); }});
  });
  require(f.store.refs().contains(a.selectors[0] + "/index") && f.store.refs().contains("observer/pin"),
          "retirement overwrote observer mutation");
  f.store.remove_ref("observer/pin");
  for (const auto &selector : {a.selectors[0], a.selectors[2]}) {
    const auto extra = selector + "/foreign";
    f.store.set_ref(extra, {Kind::revision, a.current});
    fails([&] { plan_archive_retirement(f.store, backup, a.selectors); });
    f.store.remove_ref(extra);
    const auto member = selector + (selector == a.selectors[0] ? "/index" : "/annotation");
    const auto saved = f.store.refs().at(member);
    f.store.remove_ref(member);
    fails([&] { plan_archive_retirement(f.store, backup, a.selectors); });
    f.store.set_ref(member, saved);
  }
  const auto done = retire_archives(f.store, backup, a.selectors, plan["plan"].get<std::string>());
  require(done["applied"] == true && done["removed_references"] == 11 &&
              f.store.refs() == retired_refs(refs, a.selectors) && f.store.native_head() == head &&
              text(read_bytes(f.project / "asset")) == "dirty current" &&
              text(read_bytes(f.project / "untracked")) == "leave alone",
          "group retirement changed unrelated references or workspace");
  f.store.gc(true);
  require(!f.store.contains({Kind::revision, a.tip}) && !f.store.contains({Kind::revision, a.index}) &&
              !f.store.contains({Kind::revision, a.parent}) && f.store.fsck() > 0,
          "retired group retained unreachable history after GC");
  check_bytes(backup, a.tip, "old branch tip");
  check_bytes(backup, a.index, "prepared archived index");
  check_bytes(backup, a.parent, "original");
  restore_backup(backup, root / "restored", key);
  Store restored(root / "restored", key);
  restored.restore_native_branch(a.branch);
  const auto branch = restored.native_branch_archive(a.branch, true)["archive"];
  require(branch["index"] == a.index && branch["index_base"] == a.parent && branch["stale_index"] == true &&
              restored.refs().at("staging/feature").id == a.index,
          "backup restoration lost archived stale index");
  restored.restore_native_branch(a.plain_branch);
  const auto tag = restored.restore_native_tag(a.tag)["tag"];
  require(tag["message"] == "Retained notes: \xc3\xa9" && tag["revision"] == a.tip &&
              !tag["annotation"].is_null(),
          "backup restoration lost tag annotation");
  require(restored.restore_native_tag(a.light)["tag"]["message"].is_null(),
          "backup restoration invented lightweight annotation");
  require(backup.refs() == backup_refs && read_bytes(backup.root() / "backup.json") == descriptor &&
              verify_backup(backup)["state"] == "verified",
          "retirement changed independent backup");
}
void recipe_coverage(const fs::path &root) {
  Fixture f(root / "source");
  const auto a = populate(f);
  const auto refs = f.store.refs();
  auto aliases = retired_refs(refs, a.selectors);
  // Every selected object and its full closure exists in this valid backup,
  // but its archive recipes are not rooted under their exact group names.
  unsigned i = 0;
  for (const auto &[name, key] : refs)
    if (!aliases.contains(name))
      aliases.emplace("evidence/" + std::to_string(++i), key);
  for (const auto &[name, key] : refs) {
    (void)key;
    if (!aliases.contains(name))
      f.store.remove_ref(name);
  }
  for (const auto &[name, key] : aliases)
    f.store.set_ref(name, key);
  create_backup(f.store, root / "aliases");
  for (const auto &[name, key] : aliases) {
    (void)key;
    if (!refs.contains(name))
      f.store.remove_ref(name);
  }
  for (const auto &[name, key] : refs)
    f.store.set_ref(name, key);
  Store backup(root / "aliases");
  require(verify_backup(backup)["state"] == "verified", "alias backup fixture is invalid");
  const auto before = f.physical();
  fails([&] { plan_archive_retirement(f.store, backup, a.selectors); });
  require(f.physical() == before, "missing archive recipe retirement wrote data");
}
void pagination(const fs::path &root) {
  Fixture f(root);
  const auto a = populate(f);
  const auto before = f.physical();
  const auto first = f.store.native_tag_archives({}, 1, true);
  const auto last = f.store.native_tag_archives(first["next_after"].get<std::string>(), 1, true);
  const auto lo = std::min(a.tag, a.light), hi = std::max(a.tag, a.light);
  require(first["has_more"] == true && first["archives"][0]["archive"] == lo &&
              first["objects_verified"] == true && last["archives"][0]["archive"] == hi &&
              last["has_more"] == false && last["next_after"].is_null(),
          "tag archive listing repeated groups or lost exclusive cursor");
  const auto empty = f.store.native_tag_archives(std::string(64, 'f'));
  require(empty["archives"].empty() && empty["has_more"] == false && empty["next_after"].is_null(),
          "tag archive cursor beyond inventory was not empty");
  fails([&] { f.store.native_tag_archives({}, 0); });
  fails([&] { f.store.native_tag_archives({}, 1001); });
  fails([&] { f.store.native_tag_archives("../invalid"); });
  std::stop_source stop;
  fails([&] {
    f.store.native_tag_archives({}, 100, true,
                                {stop.get_token(), [&](const ProgressEvent &) { stop.request_stop(); }});
  });
  require(f.physical() == before, "tag archive inventory wrote data");
  const auto extra = "archive/tags/" + a.tag + "/foreign";
  f.store.set_ref(extra, {Kind::revision, a.current});
  const auto malformed = f.physical();
  fails([&] { f.store.native_tag_archives(); });
  fails([&] { f.store.native_tag_archive(a.tag); });
  fails([&] { f.store.restore_native_tag(a.tag); });
  require(f.physical() == malformed, "malformed tag inventory modified evidence");
  f.store.remove_ref(extra);
  // Metadata listing does not read revision payloads, and verification is
  // scoped to the displayed page rather than unrelated later archives.
  const auto tip_manifest =
      f.store.metadata(Kind::revision, a.tip)["files"]["asset"]["manifest"].get<std::string>();
  const auto payload = f.store.metadata(Kind::manifest, tip_manifest)["blocks"][0]["id"].get<std::string>();
  require(fs::remove(f.store.root() / "objects/chunk" / payload.substr(0, 2) / payload),
          "missing tag payload fixture");
  require(f.store.native_tag_archives()["archives"].size() == 2, "metadata list required file payloads");
  fails([&] {
    f.store.native_tag_archives(a.tag < a.light ? std::optional<std::string>{} : std::optional{a.light}, 1,
                                true);
  });
  if (a.light < a.tag)
    require(f.store.native_tag_archives({}, 1, true)["archives"][0]["archive"] == a.light,
            "verification read an undisplayed archive");
}
void escaped_page_budget(const fs::path &root) {
  Fixture f(root);
  const auto target = f.revision(Json{{"asset", f.file("shared payload")}});
  const std::string message(65536, '\0');
  for (unsigned n = 0; n != 50; ++n) {
    const auto name = "label-" + std::to_string(n);
    f.store.create_native_tag(name, target, message);
    f.store.delete_native_tag(name, target);
  }
  const auto before = f.physical();
  const auto first = f.store.native_tag_archives({}, 1000);
  require(first["has_more"] == true && !first["archives"].empty() && first["archives"].size() < 50 &&
              first.dump().size() < max_object_size,
          "escaped tag archives exceeded page budget");
  const auto cursor = first["next_after"].get<std::string>();
  const auto last = f.store.native_tag_archives(cursor, 1000);
  require(last["has_more"] == false && last["archives"].size() + first["archives"].size() == 50 &&
              f.physical() == before,
          "bounded archive pages lost groups or wrote data");
  const auto prefix = "archive/tags/" + cursor + '/';
  for (const auto &[name, key] : f.store.refs()) {
    (void)key;
    if (name.starts_with(prefix))
      f.store.remove_ref(name);
  }
  require(f.store.native_tag_archives(cursor, 1000) == last, "removed archive invalidated exclusive cursor");
}
} // namespace
int main() {
  Temporary temporary;
  try {
    for (bool encrypted : {false, true})
      for (unsigned layout = 0; layout < 3; ++layout)
        retirement(temporary.path / (std::to_string(encrypted) + '-' + std::to_string(layout)), encrypted,
                   layout);
    recipe_coverage(temporary.path / "coverage");
    pagination(temporary.path / "pagination");
    escaped_page_budget(temporary.path / "escaped");
    std::cout << "Complete branch/tag retirement, independent backup recipes, pagination and retained "
                 "restoration\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
