#include "native_history_fixture.hpp"
#include "key_vault_backend.hpp"
#include <syrnck/key_vault.hpp>
#include <syrnck/sealed.hpp>
#include <syrnck/signing_keys.hpp>
#include <iostream>
#ifdef __APPLE__
#include <Security/Security.h>
#endif
using namespace native_test;
namespace {
constexpr VaultKeyPurpose purposes[]{VaultKeyPurpose::storage, VaultKeyPurpose::signing, VaultKeyPurpose::sealed};
const Bytes password(32, 19);
const Bytes message{1, 7, 3, 9, 8};
SecretKey known(VaultKeyPurpose purpose) { return SecretKey::from_bytes(Bytes(32, static_cast<std::uint8_t>(31 + static_cast<unsigned>(purpose)))); }
struct Entries {
  const SystemKeyVault &vault;
  std::vector<VaultKeyReference> refs;
  bool keep = false;
  ~Entries() {
    if (keep) return;
    for (const auto &ref : refs) try { (void)vault.erase(ref); }
      catch (const std::exception &error) { std::cerr << "owned vault fixture cleanup: " << error.what() << '\n'; }
  }
};
void references() {
  for (auto purpose : purposes) {
    const VaultKeyReference ref{purpose, std::string(64, 'a')};
    const auto encoded = ref.encode();
    const auto roundtrip = VaultKeyReference::parse(encoded);
    require(roundtrip.purpose == purpose && roundtrip.id == ref.id, "vault reference round trip changed");
    for (auto changed : {std::string{}, encoded + "\n", " " + encoded, encoded.substr(1), encoded + "0"})
      fails([&] { VaultKeyReference::parse(changed); });
    auto upper = encoded; upper.back() = 'A';
    fails([&] { VaultKeyReference::parse(upper); });
    for (const auto invalid : {'/', ':', '\\', '\0'}) {
      auto changed = encoded; changed.back() = invalid;
      fails([&] { VaultKeyReference::parse(changed); });
    }
  }
  for (auto encoded : {"syrnck-vault:2:storage:", "syrnck-vault:1:unknown:", "syrnck-vault:1:STORAGE:"})
    fails([&] { VaultKeyReference::parse(std::string(encoded) + std::string(64, '0')); });
  fails([] { (void)VaultKeyReference{static_cast<VaultKeyPurpose>(0), std::string(64, '0')}.encode(); });
  fails([] { (void)VaultKeyReference{}.encode(); });
  fails([] { (void)VaultKeyReference::parse("syrnck-vault:1:storage:" + std::string(1024 * 1024, 'a')); });
  SystemKeyVault vault;
  // Invalid inputs fail before touching an account's real vault, even in
  // provider-free builds and sessions that cannot access a system vault.
  fails([&] { vault.open_secret({VaultKeyPurpose::signing, std::string(64, '0')}); });
  fails([&] { vault.open_signer({VaultKeyPurpose::storage, std::string(64, '0')}); });
  fails([&] { vault.erase({VaultKeyPurpose::storage, "*"}); });
  fails([&] { vault.generate(static_cast<VaultKeyPurpose>(0)); });
  fails([&] { vault.import_protected({}, password, VaultKeyPurpose::storage); });
  fails([&] { vault.export_protected({VaultKeyPurpose::storage, std::string(64, '0')}, {}); });
  fails([] { SystemKeyVault invalid(fs::path("relative.keychain")); });
  fails([] {
    auto path = fs::absolute("fixture.keychain").native();
    path.push_back(fs::path::value_type{});
    SystemKeyVault invalid{fs::path(path)};
  });
  if (SystemKeyVault::provider() == "unavailable")
    fails([&] { vault.generate(VaultKeyPurpose::storage); });
#ifdef __APPLE__
  if (SystemKeyVault::provider() == "unavailable") fails([] { SystemKeyVault::disable_macos_interaction(); });
#endif
  // Validate hostile record bytes through the same decoder used for real OS
  // results. The OS vault, not this framing, authenticates secret contents.
  for (auto purpose : purposes) {
    const VaultKeyReference ref{purpose, std::string(64, 'a')};
    Bytes record{'S', 'Y', 'R', 'V', 'L', 'T', 1, 0, static_cast<std::uint8_t>(purpose)};
    const auto id = unhex(ref.id);
    record.insert(record.end(), id.begin(), id.end());
    record.insert(record.end(), 32, 71);
    const auto decoded = detail::decode_vault_record(ref, record);
    require(std::all_of(decoded.view().begin(), decoded.view().end(), [](auto byte) { return byte == 71; }),
            "vault record decoder changed payload");
    for (std::size_t at = 0; at < 41; ++at) {
      auto changed = record; changed[at] ^= 1;
      fails([&] { detail::decode_vault_record(ref, changed); });
    }
    for (std::size_t size = 0; size < record.size(); ++size)
      fails([&] { detail::decode_vault_record(ref, std::span(record).first(size)); });
    record.push_back(0);
    fails([&] { detail::decode_vault_record(ref, record); });
    wipe_secret(record);
  }
}
Json state(const fs::path &root) { return Json::parse(read_bytes(root / "state.json", 4096)); }
std::vector<VaultKeyReference> read_refs(const fs::path &root) {
  std::vector<VaultKeyReference> refs;
  const auto saved = state(root);
  for (const auto &encoded : saved.at("references")) refs.push_back(VaultKeyReference::parse(encoded.get<std::string>()));
  require(refs.size() == 3, "unexpected fixture reference count");
  return refs;
}
void create(const SystemKeyVault &vault, const fs::path &root) {
  require(SystemKeyVault::provider() != "unavailable", "real vault test requires a provider");
  Entries entries{vault, {}};
  entries.refs.reserve(3);
  Json encoded = Json::array();
  for (auto purpose : purposes) {
    entries.refs.push_back(vault.save(known(purpose), purpose));
    encoded.push_back(entries.refs.back().encode());
    const auto backup = vault.export_protected(entries.refs.back(), password);
    atomic_write(root / (std::to_string(static_cast<unsigned>(purpose)) + ".envelope"), backup, false);
  }
  // Keep a cleanup journal before any verification can fail. Only public
  // references are written, never raw keys or a user's existing credentials.
  atomic_write(root / "state.json", bytes(Json{{"references", encoded}}.dump()), false);
  entries.keep = true;
}
void verify(const SystemKeyVault &vault, const fs::path &root) {
  const auto refs = read_refs(root);
  for (std::size_t at = 0; at < refs.size(); ++at) {
    require(refs[at].purpose == purposes[at], "fixture reference purpose differs");
    auto other_purpose = refs[at];
    other_purpose.purpose = purposes[(at + 1) % 3];
    fails([&] { (void)vault.export_protected(other_purpose, password); });
    if (refs[at].purpose == VaultKeyPurpose::signing) {
      const auto signer = vault.open_signer(refs[at]);
      const auto seed = known(refs[at].purpose);
      const auto expected = SigningKey::from_seed(seed.view());
      require(signer.public_key() == expected.public_key() &&
          verify_ed25519(expected.public_key(), signer.sign(message), message), "vault signer identity changed");
      fails([&] { vault.open_secret(refs[at]); });
    } else {
      const auto opened = vault.open_secret(refs[at]);
      const auto expected = known(refs[at].purpose);
      require(std::equal(opened.view().begin(), opened.view().end(), expected.view().begin()), "vault secret changed");
      fails([&] { vault.open_signer(refs[at]); });
    }
    const auto backup = read_bytes(root / (std::to_string(static_cast<unsigned>(refs[at].purpose)) + ".envelope"), 104);
    fails([&] { vault.import_protected(backup, Bytes(32, 20), refs[at].purpose); });
    fails([&] { vault.import_protected(backup, password, other_purpose.purpose); });
    Entries imported{vault, {}};
    imported.refs.reserve(1);
    imported.refs.push_back(vault.import_protected(backup, password, refs[at].purpose));
    require(imported.refs.front().id != refs[at].id, "import reused a vault entry");
    const auto reexported = vault.export_protected(imported.refs.front(), password);
    require(reexported != backup, "export reused protected envelope salt and nonce");
    if (refs[at].purpose == VaultKeyPurpose::signing) {
      const auto signer = open_protected_signing_key(reexported, password);
      require(signer.public_key() == vault.open_signer(refs[at]).public_key(), "signer backup changed identity");
    } else if (refs[at].purpose == VaultKeyPurpose::sealed) {
      const auto opened = open_protected_sealed_key(reexported, password);
      const auto expected = known(refs[at].purpose);
      require(sealed_realm_id(opened) == sealed_realm_id(expected), "sealed backup changed realm");
    } else {
      const auto opened = open_secret_key(reexported, password);
      const auto expected = known(refs[at].purpose);
      require(std::equal(opened.view().begin(), opened.view().end(), expected.view().begin()), "storage backup changed key");
    }
    require(vault.erase(imported.refs.front()) && !vault.erase(imported.refs.front()), "vault erase was not exact/idempotent");
    fails([&] { vault.export_protected(imported.refs.front(), password); });
  }
  Entries generated{vault, {}};
  generated.refs.reserve(2);
  generated.refs.push_back(vault.generate(VaultKeyPurpose::storage));
  generated.refs.push_back(vault.generate(VaultKeyPurpose::storage));
  const auto first = vault.open_secret(generated.refs[0]), second = vault.open_secret(generated.refs[1]);
  require(generated.refs[0].id != generated.refs[1].id &&
      !std::equal(first.view().begin(), first.view().end(), second.view().begin()), "vault generation reused a key");
  for (const auto &ref : generated.refs) require(vault.erase(ref), "generated vault fixture was not erased");
  generated.refs.clear();
  // Real Store encryption consumes the native owner; no mock vault or crypto.
  auto secret = vault.open_secret(refs[0]);
  Key key{}; std::copy(secret.view().begin(), secret.view().end(), key.begin());
  Store store(root / "encrypted-store", key, true); wipe_secret(key);
  const auto object = store.put(Kind::chunk, message).first;
  require(store.encrypted() && store.get(object) == message, "vault storage owner failed to encrypt a Store");
}
void erase(const SystemKeyVault &vault, const fs::path &root) {
  const auto refs = read_refs(root);
  const auto secret = vault.open_secret(refs[0]);
  const auto signer = vault.open_signer(refs[1]);
  const auto master = vault.open_secret(refs[2]);
  for (const auto &ref : refs) {
    require(vault.erase(ref), "vault fixture entry was absent before erasure");
    require(!vault.erase(ref), "vault fixture entry remained after erasure");
    fails([&] { vault.export_protected(ref, password); });
  }
  const auto expected = known(VaultKeyPurpose::storage), expected_master = known(VaultKeyPurpose::sealed);
  require(std::equal(secret.view().begin(), secret.view().end(), expected.view().begin()) &&
      sealed_realm_id(master) == sealed_realm_id(expected_master) &&
      verify_ed25519(signer.public_key(), signer.sign(message), message), "vault erasure invalidated existing owners");
}
}
int main(int argc, char **argv) {
  try {
    if (argc == 1) { references(); std::cout << "Vault reference checks passed\n"; return 0; }
    require(argc == 3 || argc == 4, "unexpected vault test arguments");
    const SystemKeyVault vault(argc == 4 ? std::optional<fs::path>{path_from_utf8(argv[3])} : std::nullopt);
    const std::string mode = argv[1];
    if (mode != "policy-required") SystemKeyVault::disable_macos_interaction();
    const auto root = path_from_utf8(argv[2]);
    if (mode == "create") create(vault, root);
    else if (mode == "verify") verify(vault, root);
    else if (mode == "erase") erase(vault, root);
    else if (mode == "inaccessible") {
      for (const auto &ref : read_refs(root)) {
        if (ref.purpose == VaultKeyPurpose::signing) fails([&] { vault.open_signer(ref); });
        else fails([&] { vault.open_secret(ref); });
      }
    }
    else if (mode == "policy-required") {
#ifdef __APPLE__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
      require(SecKeychainSetUserInteractionAllowed(true) == errSecSuccess, "cannot set private fixture interaction policy");
#pragma clang diagnostic pop
      for (const auto &ref : read_refs(root)) {
        bool refused = false;
        try { (void)vault.export_protected(ref, password); }
        catch (const Error &error) { refused = std::string_view(error.what()) == "macOS file key vault requires explicit noninteractive process setup"; }
        require(refused, "file vault did not refuse missing explicit process policy");
      }
      Boolean interactive = false;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
      require(SecKeychainGetUserInteractionAllowed(&interactive) == errSecSuccess && interactive,
              "vault changed process interaction policy implicitly");
#pragma clang diagnostic pop
#else
      throw Error("policy-required fixture is specific to macOS");
#endif
    }
    else if (mode == "cleanup") { for (const auto &ref : read_refs(root)) (void)vault.erase(ref); }
    else throw Error("unexpected vault test mode");
    std::cout << mode << " passed using " << SystemKeyVault::provider() << '\n';
    return 0;
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
