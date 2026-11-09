#include "native_history_fixture.hpp"
#include "native_authorship.hpp"
#include <syrnck/backup.hpp>
#include <chrono>
#include <thread>
#include <iostream>
#include <type_traits>
using namespace native_test;
static_assert(!std::is_copy_constructible_v<NativeIdentityContext>);
namespace {
SigningKey signer(std::uint8_t value) { Key seed; seed.fill(value); return SigningKey::from_seed(seed); }
void signed_revision(const Store &store, const std::string &id, std::string_view author, std::string_view committer) {
  const auto value = store.metadata(Kind::revision, id);
  require(value["version"] == 2 && value["author"]["name"] == Json(author) && value["committer"]["name"] == Json(committer) &&
      value["committer"]["timestamp"] == value["timestamp"] && value["signature"]["algorithm"] == "ed25519",
      "authored revision lost its author, committer or signature");
  validate_object(Kind::revision, bytes(value.dump()));
}
void scenario(const fs::path &base, std::optional<Key> key, unsigned physical) {
  Fixture f(base / "primary", key);
  const auto first = f.revision(Json{{"code", f.file("base")}});
  f.install(first);
  const auto legacy = f.store.commit_directory(f.project, "main", "legacy");
  require(f.store.metadata(Kind::revision, legacy).size() == 5 && f.store.required_features().empty(),
      "unconfigured identity changed legacy format or enabled a feature");
  f.store.use_native_identity({"Alice Équipe", "alice@example.invalid", 60}, signer(21));
  const auto configured = f.store.native_identity();
  fails([&] { f.store.use_native_identity({"", "invalid", 0}, signer(22)); });
  require(f.store.native_identity() == configured, "invalid identity replaced the active signer");
  f.write("code", "alice");
  const auto alice = f.store.commit_directory(f.project, "main", "authored");
  signed_revision(f.store, alice, "Alice Équipe", "Alice Équipe");
  const auto features = f.store.required_features();
  require(std::find(features.begin(), features.end(), "authored-revisions-v1") != features.end(), "authored object did not gate historical readers");
  f.write("code", "prepared");
  const auto index = f.store.stage_directory(f.project, "main", {"code"});
  require(f.store.metadata(Kind::revision, index)["version"] == 1, "internal staging snapshot invented authorship");
  f.write("code", "unstaged");
  const auto staged = f.store.commit_staged("main", "prepared commit");
  signed_revision(f.store, staged, "Alice Équipe", "Alice Équipe");
  f.store.use_native_identity({"Bob", "bob@example.invalid", -120}, signer(22));
  const auto amended = f.store.amend_message("main", staged, "rewritten");
  signed_revision(f.store, amended, "Alice Équipe", "Bob");
  require(f.store.metadata(Kind::revision, amended)["signature"]["key_id"] == f.store.native_identity()["signing_key"],
      "rewriting retained the previous signer's invalid signature");
  const auto before = f.store.refs();
  f.store.clear_native_identity();
  fails([&] { f.store.amend_message("main", amended, "must identify committer"); });
  require(f.store.refs() == before, "missing rewrite identity changed history");
  f.store.use_native_identity({"Unsigned", "unsigned@example.invalid", 0});
  const auto unsigned_id = f.store.amend_message("main", amended, "unsigned rewrite");
  const auto unsigned_value = f.store.metadata(Kind::revision, unsigned_id);
  require(unsigned_value["version"] == 2 && unsigned_value["signature"].is_null() && unsigned_value["author"]["name"] == "Alice Équipe",
      "unsigned committer erased authorship or copied a signature");
  f.pack(physical); f.store.gc(true); f.store.fsck();
  create_backup(f.store, base / "backup", key);
  Store backup(base / "backup", key); verify_backup(backup);
  require(backup.required_features() == f.store.required_features(), "backup dropped the authored reader feature");
  signed_revision(backup, amended, "Alice Équipe", "Bob");
  restore_backup(backup, base / "restored", key);
  Store restored(base / "restored", key);
  require(restored.native_identity().is_null(), "backup restoration imported machine signing identity");
  signed_revision(restored, amended, "Alice Équipe", "Bob");
  restored.gc(true); restored.fsck();
  restored.export_revision(amended, base / "restored-export");
  require(text(read_bytes(base / "restored-export/code")) == "prepared", "restored signed history needed a signing private key");
  f.store.export_revision(amended, base / "export");
  require(text(read_bytes(base / "export/code")) == "prepared", "signed metadata changed file reconstruction");
}
void malformed(const fs::path &base) {
  Fixture f(base);
  f.store.use_native_identity({"Alice", "alice@example.invalid", 0}, signer(23));
  const Json draft{{"version", 1}, {"parents", Json::array()}, {"message", "signed test"}, {"timestamp", 42}, {"files", Json::object()}};
  const auto value = f.store.native_commit_metadata(draft);
  const auto storage = f.physical();
  const auto reject = [&](Json changed) {
    fails([&] { f.store.put(Kind::revision, bytes(changed.dump())); });
    require(f.physical() == storage && f.store.required_features().empty(), "invalid signed input wrote objects or configuration");
  };
  auto changed = value; changed["message"] = "tampered"; reject(changed);
  changed = value; changed["author"]["name"] = "forged"; reject(changed);
  changed = value; changed["committer"]["timezone_offset"] = 15; reject(changed);
  changed = value; changed["signature"]["key_id"] = "ed25519:" + std::string(64, '0'); reject(changed);
  changed = value; changed["signature"]["value"] = std::string(128, '0'); reject(changed);
  changed = value; changed["signature"]["algorithm"] = "unknown"; reject(changed);
  changed = value; changed["author"]["timezone_offset"] = UINT64_MAX; reject(changed);
  changed = value; changed["author"]["name"] = "line\nbreak"; reject(changed);
  changed = value; changed["committer"]["timestamp"] = 41; reject(changed);
  changed = value; changed["extra"] = true; reject(changed);
  require(f.store.native_commit_metadata(draft, Json{})["author"].is_null(), "legacy rewrite fabricated an original author");
  f.store.clear_native_identity();
  require(f.store.native_commit_metadata(draft) == draft, "legacy producer metadata changed without identity");
  const auto id = f.store.put(Kind::revision, bytes(value.dump())).first.id;
  f.store.set_ref("heads/main", {Kind::revision, id}); f.store.gc(true); f.store.fsck();
}
void rebase(const fs::path &base, bool fixed_clock) {
  Fixture f(base);
  const auto original = f.revision(Json{{"code", f.file("base")}});
  const auto onto = f.revision(Json{{"code", f.file("base")}, {"onto", f.file("new base")}}, Json::array({original}));
  f.install(original);
  f.store.use_native_identity({"Alice", "alice@example.invalid", 60}, signer(24));
  f.write("code", "picked");
  const auto picked = f.store.commit_directory(f.project, "main", "picked author");
  const auto source_author = f.store.metadata(Kind::revision, picked)["author"];
  f.store.use_native_identity({"Bob", "bob@example.invalid", -60}, signer(25));
  NativeRebaseOptions settings;
  if (fixed_clock) settings.committer_timestamp = 1234;
  const auto before = f.physical();
  const auto plan = f.store.plan_rebase(f.project, "main", original, onto, settings, true);
  require(plan["ready"] == true && f.physical() == before, "signed rebase preview wrote objects or blocked clean work");
  bool paused = false;
  const auto result = f.store.rebase_directory(f.project, "main", picked, original, onto, settings,
      {{}, [&](const ProgressEvent &event) {
        if (!paused && event.phase == ProgressPhase::planning && event.complete) {
          paused = true;
          fails([&] { f.store.clear_native_identity(); });
          fails([&] { (void)f.store.take_native_identity(); });
          if (!fixed_clock) std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        }
      }});
  const auto target = result["target"].get<std::string>();
  signed_revision(f.store, target, "Alice", "Bob");
  const auto value = f.store.metadata(Kind::revision, target);
  require(paused && value["author"] == source_author && value["files"].contains("onto"), "rebase lost original author or combined files");
  if (fixed_clock) require(value["timestamp"] == 1234 && target == plan["target"].get<std::string>(), "signed rebase did not preserve its frozen clock and predicted ID");
  f.store.gc(true); f.store.fsck();
}
void producers(const fs::path &base) {
  Fixture f(base);
  const auto original = f.revision(Json{{"code", f.file("base")}});
  f.install(original);
  f.store.use_native_identity({"Alice", "alice@example.invalid", 60}, signer(26));
  f.write("code", "incoming");
  const auto incoming = f.store.commit_directory(f.project, "main", "Alice work");
  const auto author = f.store.metadata(Kind::revision, incoming)["author"];
  f.store.create_native_branch("feature", incoming);
  f.install(original);
  f.store.use_native_identity({"Bob", "bob@example.invalid", -60}, signer(27));
  f.write("own", "independent");
  auto head = f.store.commit_directory(f.project, "main", "Bob work");
  const auto picked = f.store.replay_directory(f.project, "main", head, incoming, NativeReplayKind::cherry_pick, "picked")["target"].get<std::string>();
  signed_revision(f.store, picked, "Alice", "Bob");
  require(f.store.metadata(Kind::revision, picked)["author"] == author, "cherry-pick changed exact source author stamp");
  const auto reverted = f.store.replay_directory(f.project, "main", picked, incoming, NativeReplayKind::revert, "reverted")["target"].get<std::string>();
  signed_revision(f.store, reverted, "Bob", "Bob");
  f.install(head);
  const auto merged = f.store.merge_directory(f.project, "main", head, incoming, "merged")["target"].get<std::string>();
  signed_revision(f.store, merged, "Bob", "Bob");
  require(f.store.metadata(Kind::revision, merged)["parents"] == Json::array({head, incoming}), "signed merge lost parent topology");
  f.install(picked);
  const auto squashed = f.store.squash_commits("main", picked, 2, "combined")["target"].get<std::string>();
  signed_revision(f.store, squashed, "Bob", "Bob");
  const auto detached = f.store.detach_history("main", "detached history");
  signed_revision(f.store, detached, "Bob", "Bob");
  require(f.store.metadata(Kind::revision, detached)["author"] == f.store.metadata(Kind::revision, squashed)["author"], "history detachment changed original author stamp");
  f.store.gc(true); f.store.fsck();
  for (const auto &id : {incoming, picked, reverted, merged, squashed, detached})
    (void)f.store.closure({{Kind::revision, id}}, true);
}
void identity_session(const fs::path &base) {
  const auto expected_key = signing_key_id(signer(28).public_key());
  std::optional<NativeIdentityContext> context{NativeIdentityContext{{"Resident", "resident@example.invalid", 90}, signer(28), true}};
  std::string previous;
  for (unsigned index = 0; index < 3; ++index) {
    Fixture f(base / std::to_string(index));
    f.write("code", "same process, a fresh Store handle");
    f.store.use_native_identity(std::move(context->identity), std::move(context->signer), context->require_signature);
    context.reset();
    const auto revision = f.store.commit_directory(f.project, "main", "resident commit");
    signed_revision(f.store, revision, "Resident", "Resident");
    require(f.store.native_identity()["signing_key"] == expected_key, "resident identity lost its original signing key");
    context = f.store.take_native_identity();
    require(context && context->require_signature && context->identity.timezone_offset == 90 && context->signer &&
        signing_key_id(context->signer->public_key()) == expected_key && f.store.native_identity().is_null(), "taking identity did not transfer exclusive ownership");
    require(!f.store.take_native_identity(), "empty identity returned a second context");
    previous = revision;
  }
  Fixture f(base / "unsigned");
  f.store.use_native_identity({"Unsigned", "unsigned@example.invalid", 0});
  auto unsigned_context = f.store.take_native_identity();
  require(unsigned_context && !unsigned_context->signer && unsigned_context->identity.name == "Unsigned", "unsigned identity could not move between handles");
  require(context->signer->sign(bytes(previous)).size() == 64, "transferred signing key was wiped by a former Store destructor");
}
}
int main() {
  Temporary temporary;
  try {
    for (unsigned physical = 0; physical < 3; ++physical) for (bool encrypted : {false, true}) {
      std::optional<Key> key; if (encrypted) { key.emplace(); key->fill(87); }
      scenario(temporary.path / (std::to_string(physical) + '-' + std::to_string(encrypted)), key, physical);
    }
    malformed(temporary.path / "malformed");
    rebase(temporary.path / "rebase-fixed", true); rebase(temporary.path / "rebase-clock", false);
    producers(temporary.path / "producers");
    identity_session(temporary.path / "identity-session");
    std::cout << "Authored signatures preserve original authors, independent committers and stable rewrite candidates\n";
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
