#include "native_history_fixture.hpp"
#include "server_access.hpp"
#include "signing_validation.hpp"
#include <iostream>
using namespace native_test;
using namespace syrnck::net::detail;
namespace {
SigningKey signer(std::uint8_t value) { Key seed; seed.fill(value); return SigningKey::from_seed(seed); }
Json binding(std::uint8_t seed, std::string name) {
  return Json{{"public_key", hex(signer(seed).public_key())}, {"name", name}, {"email", name + "@example.invalid"}};
}
Json configuration(bool required = true) {
  Json users = Json::array();
  for (const auto &name : {"Alice", "Bob", "Reader"}) {
    const bool read = std::string_view(name) == "Reader";
    users.push_back(Json{{"id", name}, {"token_hash", hash(bytes(name))}, {"role", read ? "reader" : "writer"},
        {"write_paths", read ? Json::array() : Json::array({"Source"})}, {"branches", read ? Json::array() : Json::array({"main", "scratch"})},
        {"signing_keys", read ? Json::array() : Json::array({binding(std::string_view(name) == "Alice" ? 41 : 42, name)})}});
  }
  return Json{{"version", 2}, {"principals", users}, {"branches", Json::array({
      Json{{"name", "main"}, {"fast_forward", true}, {"lock_paths", Json::array()}, {"require_signatures", required}},
      Json{{"name", "scratch"}, {"fast_forward", false}, {"lock_paths", Json::array()}, {"require_signatures", required}}})}};
}
void invalid_configuration() {
  const auto good = configuration();
  ServerAccess access(good);
  require(access.authenticate("Alice")->id == "Alice", "signed policy changed token identity");
  for (const auto &field : {"signing_keys", "extra"}) {
    auto bad = good; bad["principals"][0][field] = nullptr; fails([&] { ServerAccess rejected(bad); });
  }
  auto bad = good; bad["principals"][0].erase("signing_keys"); fails([&] { ServerAccess rejected(bad); });
  bad = good; bad["principals"][1]["signing_keys"] = bad["principals"][0]["signing_keys"]; fails([&] { ServerAccess rejected(bad); });
  bad = good; bad["principals"][0]["signing_keys"].push_back(bad["principals"][0]["signing_keys"][0]); fails([&] { ServerAccess rejected(bad); });
  bad = good; bad["principals"][2]["signing_keys"] = Json::array({binding(43, "Reader")}); fails([&] { ServerAccess rejected(bad); });
  // Canonical y=2 has no corresponding curve point; y=0, 1 and -1 are weak.
  for (const auto &key : {std::string(64, '0'), std::string("01") + std::string(62, '0'),
      std::string("02") + std::string(62, '0'), std::string("ec") + std::string(60, 'f') + "7f", std::string(64, 'f'), std::string(62, '0')}) {
    bad = good; bad["principals"][0]["signing_keys"][0]["public_key"] = key; fails([&] { ServerAccess rejected(bad); });
  }
  bad = good; bad["principals"][0]["signing_keys"][0]["email"] = "bad\naddress"; fails([&] { ServerAccess rejected(bad); });
  bad = good; bad["principals"][0]["signing_keys"][0]["extra"] = true; fails([&] { ServerAccess rejected(bad); });
  bad = good; bad["branches"][0]["require_signatures"] = "yes"; fails([&] { ServerAccess rejected(bad); });
  bad = good; bad["branches"][0].erase("require_signatures"); fails([&] { ServerAccess rejected(bad); });
  bad = good; bad["version"] = 1; fails([&] { ServerAccess rejected(bad); });
  require(valid_ed25519_public_key(signer(41).public_key()) && !valid_ed25519_public_key(Bytes(31)), "configuration key validation rejected an owned key or a malformed size");
}
struct History {
  Fixture f;
  std::string base;
  explicit History(const fs::path &root, std::optional<Key> key = {}) : f(root, key) {
    base = f.revision(Json{{"Source/code", f.file("legacy")}});
    f.store.set_ref("main", {Kind::revision, base});
  }
  std::string revision(std::string parent, std::string person, std::uint8_t seed,
      std::string path = "Source/code", bool signed_value = true) {
    auto draft = f.store.metadata(Kind::revision, parent);
    draft["parents"] = Json::array({parent}); draft["message"] = "changed by " + person;
    draft["timestamp"] = ++f.sequence;
    draft["files"][path] = f.file("new bytes " + std::to_string(f.sequence));
    f.store.use_native_identity({person, person + "@example.invalid", 0}, signed_value ? std::optional{signer(seed)} : std::nullopt);
    // A new commit has a fresh author, irrespective of the selected parent's metadata.
    return f.store.put(Kind::revision, bytes(f.store.native_commit_metadata(std::move(draft)).dump())).first.id;
  }
};
void policy(const fs::path &root, std::optional<Key> key, unsigned layout) {
  History h(root, key);
  const auto alice = h.revision(h.base, "Alice", 41);
  const auto bob = h.revision(alice, "Bob", 42);
  h.f.pack(layout);
  const auto config = configuration();
  ServerAccess access(config);
  const auto &publisher = *access.authenticate("Bob");
  const auto before = h.f.physical();
  const auto accept = [&](const std::string &id, const std::string &previous, const ServerAccess &policy, std::string_view branch = "main") {
    const auto &actor = *policy.authenticate("Bob");
    (void)policy.publication(h.f.store, actor, branch, {Kind::revision, id}, ObjectKey{Kind::revision, previous}, {});
  };
  accept(alice, h.base, access); accept(bob, h.base, access);
  require(h.f.physical() == before, "signature authorization planning wrote objects or references");
  // Legacy ancestry is admitted only through this branch's existing baseline.
  fails([&] { access.publication(h.f.store, publisher, "main", {Kind::revision, bob}, {}, {}); });
  accept(h.base, h.base, access); // Exact retry of an already admitted legacy tip.
  fails([&] { accept(h.base, bob, access, "scratch"); }); // Signed-policy rewind cannot downgrade to legacy.
  accept(alice, bob, access, "scratch");
  const auto unsigned_id = h.revision(h.base, "Alice", 41, "Source/code", false);
  const auto unknown = h.revision(h.base, "Alice", 43);
  const auto wrong_name = h.revision(h.base, "Mallory", 41);
  for (const auto &id : {unsigned_id, unknown, wrong_name}) {
    fails([&] { accept(id, h.base, access); });
    // A correct tip cannot hide the unapproved intermediate metadata.
    const auto correct_tip = h.revision(id, "Bob", 42);
    fails([&] { accept(correct_tip, h.base, access); });
  }
  h.f.store.use_native_identity({"Bob", "Bob@example.invalid", 0}, signer(42));
  auto draft = h.f.store.metadata(Kind::revision, bob);
  draft["parents"] = Json::array({alice, unsigned_id});
  const auto merged = h.f.store.put(Kind::revision, bytes(h.f.store.native_commit_metadata(draft).dump())).first.id;
  fails([&] { accept(merged, h.base, access); }); // An unsigned second merge parent cannot be hidden.
  draft["parents"] = Json::array({h.base});
  auto original_author = draft["author"]; original_author["name"] = "Historical claim";
  const auto attributed = h.f.store.put(Kind::revision, bytes(h.f.store.native_commit_metadata(draft, original_author).dump())).first.id;
  accept(attributed, h.base, access); // Authorization applies to the actual committer, not a historical author claim.
  h.f.store.use_native_identity({"Alice", "wrong@example.invalid", 0}, signer(41));
  const auto email_mismatch = h.f.store.put(Kind::revision, bytes(h.f.store.native_commit_metadata(draft).dump())).first.id;
  fails([&] { accept(email_mismatch, h.base, access); });
  // Per-signer branch/path restrictions apply in addition to publisher rights.
  auto reduced = config; reduced["principals"][0]["branches"] = Json::array({"scratch"});
  ServerAccess branch_policy(reduced); fails([&] { accept(alice, h.base, branch_policy); });
  reduced = config; reduced["principals"][0]["write_paths"] = Json::array({"Content"});
  ServerAccess path_policy(reduced); fails([&] { accept(alice, h.base, path_policy); });
  reduced = config; reduced["principals"][1]["write_paths"] = Json::array({"Content"});
  ServerAccess publisher_policy(reduced); fails([&] { accept(alice, h.base, publisher_policy); });
  // Removing a key revokes new publications, without invalidating admitted ancestry.
  reduced = config; reduced["principals"][0]["signing_keys"] = Json::array();
  ServerAccess revoked(reduced);
  fails([&] { accept(alice, h.base, revoked); });
  fails([&] { accept(alice, bob, revoked, "scratch"); });
  accept(alice, alice, revoked); accept(bob, alice, revoked);
  reduced = config; reduced["principals"][0]["signing_keys"].push_back(binding(44, "Alice"));
  ServerAccess rotating(reduced);
  const auto rotated = h.revision(alice, "Alice", 44);
  accept(rotated, h.base, rotating);
  fails([&] { accept(rotated, h.base, access); });
  reduced["principals"][0]["signing_keys"].erase(0);
  ServerAccess after_rotation(reduced);
  accept(rotated, alice, after_rotation);
  fails([&] { accept(rotated, h.base, after_rotation); });
  require(h.f.store.refs() == Refs{{"main", {Kind::revision, h.base}}}, "signature-policy refusal changed published references");
  // Both objects and signatures remain readable after revocation and packing.
  const auto callback = access.publication(h.f.store, publisher, "main", {Kind::revision, bob}, ObjectKey{Kind::revision, h.base}, {});
  require(h.f.store.compare_update_refs({{"main", ObjectKey{Kind::revision, h.base}, ObjectKey{Kind::revision, bob}}}, {{}, callback}), "authorized signed publication failed CAS");
  h.f.store.clear_native_identity(); h.f.store.gc(true); h.f.store.fsck();
  require(h.f.store.metadata(Kind::revision, alice)["signature"]["key_id"] == signing_key_id(signer(41).public_key()), "key revocation modified retained signature bytes");
  ServerAccess permissive(configuration(false));
  (void)permissive.publication(h.f.store, *permissive.authenticate("Bob"), "scratch", {Kind::revision, h.base}, {}, {});
}
}
int main() {
  Temporary temporary;
  try {
    invalid_configuration();
    for (unsigned layout = 0; layout < 3; ++layout) for (bool encrypted : {false, true}) {
      std::optional<Key> key; if (encrypted) { key.emplace(); key->fill(91); }
      policy(temporary.path / (std::to_string(layout) + '-' + std::to_string(encrypted)), key, layout);
    }
    std::cout << "Signed publication authorizes all new committers, independent publishers, ACLs and current key rotation policies\n";
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
