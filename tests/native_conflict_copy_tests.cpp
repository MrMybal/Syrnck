#include "native_history_fixture.hpp"
#include "native_merge.hpp"
#include <syrnck/native_sync.hpp>
#include <iostream>
using namespace native_test;
namespace {
std::string phase;
void preservation(const fs::path &path, std::optional<Key> key, unsigned layout) {
  phase = "create " + utf8(path);
  Fixture f(path, key);
  const auto base_file = f.file("base"), a = f.file("local asset"), b = f.file("remote asset");
  const auto base = f.revision(Json{{"Content/Map.uasset", base_file}, {"text", f.file("one\nkeep\nlast\n")},
      {"modify_delete", base_file}, {"delete_modify", base_file}, {"both_deleted", base_file}});
  const auto left = f.revision(Json{{"Content/Map.uasset", a}, {"text", f.file("ONE\nkeep\nlast\n")},
      {"added", a}, {"modify_delete", a}}, Json::array({base}));
  const auto right = f.revision(Json{{"Content/Map.uasset", b}, {"text", f.file("one\nkeep\nLAST\n")},
      {"added", b}, {"delete_modify", b}}, Json::array({base}));
  phase = "install/pack " + utf8(path);
  f.install(left); f.store.set_ref("incoming", {Kind::revision, right}); f.pack(layout);
  NativeMergeOptions settings; settings.conflict_copies = true; settings.text_merge = TextDiffOptions{};
  const auto before = f.physical();
  require(!f.store.plan_merge(path, "main", right)["ready"].get<bool>(), "default merge silently copied conflicts");
  phase = "plan " + utf8(path);
  const auto plan = prepare_native_merge(f.store, path, "main", right, settings, true, {});
  require(plan.report["ready"] == true && plan.report["resolved"].size() == 4 && plan.files.size() == 7 &&
      !plan.report["text_merge"]["files"].empty() && f.physical() == before,
      "conflict-copy planning changed data or failed to preserve files/text");
  std::string copy;
  for (const auto &row : plan.report["resolved"]) {
    if (row["choice"] != "conflict_copy") continue;
    const auto original = row["path"].get<std::string>(), sibling = row["copy_path"].get<std::string>();
    require(plan.files.contains(sibling) && row["copy_revision"] == (left < right ? right : left),
        "copy naming depends on local peer orientation");
    require(std::any_of(plan.report["changes"].begin(), plan.report["changes"].end(), [&](const Json &change) {
        return change["path"] == sibling && change["action"] == "create";
      }), "copy absent from proposed checkout changes");
    if (original == "Content/Map.uasset") copy = sibling;
  }
  require(!copy.empty() && copy.ends_with(".uasset") && plan.files.contains("modify_delete") &&
      plan.files.contains("delete_modify") && !plan.files.contains("both_deleted"),
      "conflict policy lost asset extension, modification or deletion semantics");
  phase = "obstruction " + utf8(path);
  f.write(path_from_utf8(copy), "untracked obstruction");
  const auto obstructed = f.physical();
  require(f.store.plan_merge(path, "main", right, settings)["ready"] == false && f.physical() == obstructed,
      "conflict copy accepted an untracked obstruction");
  fs::remove(path / path_from_utf8(copy));
  phase = "reverse " + utf8(path);
  f.install(right);
  const auto reverse = prepare_native_merge(f.store, path, "main", left, settings, true, {});
  require(reverse.report["ready"] == true && reverse.files.size() == plan.files.size() &&
      std::equal(reverse.files.begin(), reverse.files.end(), plan.files.begin(), [](const auto &a, const auto &b) {
        return a.first == b.first && a.second.json() == b.second.json();
      }),
      "opposite peers computed different merged file namespaces");
  phase = "apply " + utf8(path);
  f.install(left);
  Key seed; seed.fill(56);
  f.store.use_native_identity({"Conflict test", "copy@example.invalid", 0}, SigningKey::from_seed(seed));
  settings.checkout_owner = std::string(64, 'a');
  std::stop_source stop;
  fails([&] { f.store.merge_directory(path, "main", left, right, "preserve concurrent assets", settings,
      {stop.get_token(), [&](const ProgressEvent &event) {
        if (event.phase == ProgressPhase::exporting && event.bytes > 0) stop.request_stop();
      }}); });
  const auto frozen = f.store.checkout_status();
  require(frozen["pending"] == true && frozen["owner"] == settings.checkout_owner,
      "conflict merge did not retain its signed recoverable candidate");
  const auto target = frozen["target"].get<std::string>();
  (void)f.store.take_native_identity();
  phase = "recover " + utf8(path);
  f.store.recover_checkout_owned(path, "main", *settings.checkout_owner);
  const auto revision = f.store.metadata(Kind::revision, target);
  require(revision["parents"] == Json::array({left, right}) && revision["signature"].is_object() &&
      f.store.native_head()["revision"] == target && f.store.native_identity().is_null() &&
      f.store.workspace_status(path, "main")["clean"] == true &&
      text(read_bytes(path / "text")) == "ONE\nkeep\nLAST\n" &&
      text(read_bytes(path / path_from_utf8(copy))) == (left < right ? "remote asset" : "local asset"),
      "keyless recovery changed a signed conflict candidate or lost bytes");
  f.store.remove_ref("incoming"); f.store.gc(true); f.store.fsck();
  for (const auto &id : {base, left, right}) (void)f.store.closure({{Kind::revision, id}}, true);
}
void collisions(const fs::path &path) {
  phase = "collisions " + utf8(path);
  Fixture f(path);
  const auto a = f.file("left"), b = f.file("right"), original = f.file("base");
  NativeConflictCopyContext context{std::string(64, '1'), std::string(64, '2')};
  const std::string name = "Content/Map.uasset";
  auto current = read_native_snapshot(f.store, f.revision(Json{{name, a}}), {});
  const auto incoming = read_native_snapshot(f.store, f.revision(Json{{name, b}}), {});
  const auto base = read_native_snapshot(f.store, f.revision(Json{{name, original}}), {});
  const auto empty = merge_native_files(f.store, base, current, incoming, {}, nullptr, nullptr, nullptr, &context);
  const auto copy = empty.resolved[0]["copy_path"].get<std::string>();
  for (const auto &obstruction : {copy, copy + "/leaf"}) {
    auto collision = current;
    collision.emplace(obstruction, current.begin()->second);
    const auto result = merge_native_files(f.store, base, collision, incoming, {}, nullptr, nullptr, nullptr, &context);
    require(result.conflicts.size() == 1 && result.conflicts[0]["reason"] == "conflict_copy_collision" &&
        result.files.contains(obstruction) && result.resolved.empty(), "copy overwrote an input file/directory");
  }
  // A copied name that differs only by case remains occupied on every platform.
  auto folded = copy; folded[0] = 'c'; current.emplace(folded, current.begin()->second);
  require(merge_native_files(f.store, base, current, incoming, {}, nullptr, nullptr, nullptr, &context)
      .conflicts[0]["reason"] == "conflict_copy_collision", "copy ignored a portable case collision");
  const auto left = f.revision(Json{{"shape", a}}), right = f.revision(Json{{"shape/leaf", b}});
  f.install(left); NativeMergeOptions settings; settings.conflict_copies = true; settings.allow_unrelated_histories = true;
  require(f.store.plan_merge(path, "main", right, settings)["ready"] == false,
      "copy policy silently resolved a namespace topology conflict");
  std::string multibyte; for (unsigned n = 0; n < 70; ++n) multibyte += "\xC3\xA9";
  for (const auto &name : {std::string(230, 'x') + ".uasset", multibyte + ".uasset"}) {
    const auto source = f.revision(Json{{name, original}});
    const auto ours = f.revision(Json{{name, a}}, Json::array({source}));
    const auto theirs = f.revision(Json{{name, b}}, Json::array({source}));
    const auto combined = merge_native_files(f.store, read_native_snapshot(f.store, source, {}),
        read_native_snapshot(f.store, ours, {}), read_native_snapshot(f.store, theirs, {}),
        {}, nullptr, nullptr, nullptr, &context);
    const auto sibling = combined.resolved[0]["copy_path"].get<std::string>();
    require(combined.conflicts.empty() && sibling.size() < 255 && !path_from_utf8(sibling).empty(),
        "conflict naming exceeded the portable component limit");
  }
  net::NativeSyncOptions options; options.conflict_copies = true;
  fails([&] { net::NativeSync sync(f.store, path, options); });
}
}
int main() {
  Temporary temporary;
  try {
    for (bool encrypted : {false, true}) for (unsigned layout = 0; layout < 3; ++layout) {
      std::optional<Key> key; if (encrypted) { key.emplace(); key->fill(57); }
      preservation(temporary.path / (std::to_string(encrypted) + '-' + std::to_string(layout)), key, layout);
    }
    collisions(temporary.path / "collisions");
    std::cout << "Deterministic conflict copies preserve data, reject collisions and recover signed targets\n";
  } catch (const std::exception &error) { std::cerr << phase << ": " << error.what() << '\n'; return 1; }
}
