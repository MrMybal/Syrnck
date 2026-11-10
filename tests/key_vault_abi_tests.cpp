#include "native_history_fixture.hpp"
#include <syrnck.h>
#include <syrnck/key_vault.hpp>
#include <syrnck/signing_keys.hpp>
#include <syrnck/sealed.hpp>
#include <iostream>
#ifdef __APPLE__
#include <Security/Security.h>
#endif
using namespace native_test;
namespace {
const Bytes password(32, 27), message{1, 9, 4, 8, 7};
void ok(int code) { require(code == 0, syrnck_last_error()); }
struct Vault {
  SyrnckSystemVault *value = nullptr;
  explicit Vault(const char *path = nullptr) { ok(syrnck_system_vault_open(path, &value)); }
  ~Vault() { syrnck_system_vault_close(value); }
};
struct Secret { SyrnckSecretKey *value = nullptr; ~Secret() { syrnck_secret_key_close(value); } };
struct Signer { SyrnckSigningKey *value = nullptr; ~Signer() { syrnck_signing_key_close(value); } };
struct Master { SyrnckSealedKey *value = nullptr; ~Master() { syrnck_sealed_key_close(value); } };
struct CStore { SyrnckStore *value = nullptr; ~CStore() { syrnck_store_close(value); } };
Json allocated_json(int code, char *value, std::size_t count) {
  struct Free { char *value; ~Free() { syrnck_string_free(value); } } free{value};
  ok(code); require(value && count && value[count - 1] == 0, "missing ABI JSON");
  return Json::parse(std::string_view(value, count - 1));
}
Json provider(Vault &vault) {
  char *value = nullptr; std::size_t size = 0;
  const auto code = syrnck_system_vault_provider_json(vault.value, &value, &size);
  return allocated_json(code, value, size);
}
Json metadata(Signer &key) {
  char *value = nullptr; std::size_t size = 0;
  const auto code = syrnck_signing_key_public_json(key.value, &value, &size);
  return allocated_json(code, value, size);
}
Json metadata(Master &key) {
  char *value = nullptr; std::size_t size = 0;
  const auto code = syrnck_sealed_key_public_json(key.value, &value, &size);
  return allocated_json(code, value, size);
}
Bytes backup(Vault &vault, const std::string &ref) {
  Bytes out(104); std::size_t size = 0;
  ok(syrnck_system_vault_export_protected(vault.value, ref.c_str(), password.data(), password.size(), nullptr,
      out.data(), out.size(), &size));
  require(size == out.size(), "ABI vault export length changed");
  return out;
}
std::string generate(Vault &vault, unsigned purpose) {
  std::array<char, SYRNCK_VAULT_REFERENCE_CAPACITY> out{}; std::size_t size = 0;
  ok(syrnck_system_vault_generate(vault.value, purpose, out.data(), out.size(), &size));
  require(size && size < out.size() && out[size] == 0, "invalid ABI vault reference length");
  const auto ref = VaultKeyReference::parse(out.data());
  require(static_cast<unsigned>(ref.purpose) == purpose && ref.encode().size() == size, "ABI vault reference changed");
  return ref.encode();
}
void remove(Vault &vault, const std::string &ref, bool expected) {
  bool removed = !expected;
  ok(syrnck_system_vault_erase(vault.value, ref.c_str(), &removed));
  require(removed == expected, "ABI vault erasure result changed");
}
std::vector<std::string> refs(const fs::path &root) {
  const auto state = Json::parse(read_bytes(root / "state.json", 4096));
  return state.at("references").get<std::vector<std::string>>();
}
void basic() {
  Vault vault;
  require(provider(vault).at("version") == 1, "invalid ABI provider metadata");
  const auto ref = "syrnck-vault:1:storage:" + std::string(64, 'a');
  auto *invalid = reinterpret_cast<SyrnckSystemVault *>(1);
  require(syrnck_system_vault_open("relative", &invalid) != 0 && !invalid, "ABI invalid open retained output");
  std::array<char, 97> output; output.fill('q'); std::size_t size = 123;
  require(syrnck_system_vault_generate(vault.value, 0, output.data(), output.size(), &size) != 0 && !size && !output[0],
      "ABI invalid purpose retained output");
  output.fill('q'); size = 123;
  require(syrnck_system_vault_generate(vault.value, 1, output.data(), 96, &size) != 0 && !size && !output[0] && output[1] == 'q' &&
      std::string_view(syrnck_last_error()) == "insufficient output capacity",
      "ABI short reference output wrote or generated a key");
  require(syrnck_system_vault_generate(vault.value, 1, nullptr, 0, &size) != 0 && !size,
      "ABI creation accepted a size query");
  Secret key; Signer signer; Master master;
  key.value = reinterpret_cast<SyrnckSecretKey *>(1);
  require(syrnck_system_vault_open_secret(nullptr, ref.c_str(), &key.value) != 0 && !key.value, "ABI null vault retained secret");
  signer.value = reinterpret_cast<SyrnckSigningKey *>(1);
  require(syrnck_system_vault_open_signer(vault.value, ref.c_str(), &signer.value) != 0 && !signer.value,
      "ABI storage reference became a signer");
  master.value = reinterpret_cast<SyrnckSealedKey *>(1);
  require(syrnck_system_vault_open_sealed(vault.value, ref.c_str(), &master.value) != 0 && !master.value,
      "ABI storage reference became a master");
  // A size query must not access the real account vault, even for an absent
  // reference, provider-free build or macOS process without headless setup.
  ok(syrnck_system_vault_export_protected(vault.value, ref.c_str(), password.data(), password.size(), nullptr,
      nullptr, 0, &size));
  require(size == 104, "ABI export query did not return fixed size");
  size = 123;
  require(syrnck_system_vault_export_protected(vault.value, ref.c_str(), nullptr, 0, nullptr, nullptr, 0, &size) != 0 && !size,
      "ABI export accepted empty password");
  bool removed = true;
  require(syrnck_system_vault_erase(vault.value, "*", &removed) != 0 && !removed, "ABI erasure retained failure output");
  char *json = reinterpret_cast<char *>(1); size = 123;
  require(syrnck_system_vault_provider_json(nullptr, &json, &size) != 0 && !json && !size,
      "ABI provider failure retained output");
  ok(syrnck_system_vault_close(nullptr));
}
void create(Vault &vault, const fs::path &root) {
  std::vector<std::string> entries;
  entries.reserve(3);
  try {
    for (unsigned purpose = 1; purpose <= 3; ++purpose) {
      entries.push_back(generate(vault, purpose));
      atomic_write(root / "state.json", bytes(Json{{"references", entries}}.dump()), entries.size() != 1);
      atomic_write(root / (std::to_string(purpose) + ".envelope"), backup(vault, entries.back()), false);
    }
  } catch (...) {
    for (const auto &ref : entries) { bool removed = false; (void)syrnck_system_vault_erase(vault.value, ref.c_str(), &removed); }
    throw;
  }
}
void verify(Vault &vault, const fs::path &root) {
  const auto entries = refs(root); require(entries.size() == 3, "missing ABI vault fixture references");
  Secret key; Signer signer; Master master;
  ok(syrnck_system_vault_open_secret(vault.value, entries[0].c_str(), &key.value));
  ok(syrnck_system_vault_open_signer(vault.value, entries[1].c_str(), &signer.value));
  ok(syrnck_system_vault_open_sealed(vault.value, entries[2].c_str(), &master.value));
  const auto signer_metadata = metadata(signer), realm = metadata(master);
  for (unsigned purpose = 1; purpose <= 3; ++purpose) {
    const auto envelope = read_bytes(root / (std::to_string(purpose) + ".envelope"), 104);
    std::array<char, 97> imported{}; std::size_t size = 0;
    ok(syrnck_system_vault_import_protected(vault.value, purpose, envelope.data(), envelope.size(),
        password.data(), password.size(), imported.data(), imported.size(), &size));
    const std::string ref(imported.data(), size);
    try {
      require(ref != entries[purpose - 1], "ABI import reused an entry");
      const auto reexported = backup(vault, ref);
      require(reexported != envelope, "ABI export reused nonce/salt");
      if (purpose == SYRNCK_VAULT_SIGNING) {
        const auto reopened = open_protected_signing_key(reexported, password);
        require(hex(reopened.public_key()) == signer_metadata.at("public_key").get<std::string>(), "ABI signer backup changed identity");
      } else if (purpose == SYRNCK_VAULT_SEALED) {
        const auto reopened = open_protected_sealed_key(reexported, password);
        require(sealed_realm_id(reopened) == realm.at("realm").get<std::string>(), "ABI sealed backup changed identity");
      }
      remove(vault, ref, true); remove(vault, ref, false);
    } catch (...) { bool removed = false; (void)syrnck_system_vault_erase(vault.value, ref.c_str(), &removed); throw; }
    require(syrnck_system_vault_import_protected(vault.value, purpose, envelope.data(), envelope.size(),
        message.data(), message.size(), imported.data(), imported.size(), &size) != 0 && !size && !imported[0],
        "ABI import accepted wrong password or retained output");
  }
  // Key owners are independent of the provider configuration's lifetime.
  ok(syrnck_system_vault_close(vault.value)); vault.value = nullptr;
  CStore store;
  const auto path = utf8(root / "store");
  ok(syrnck_store_open_secret(path.c_str(), true, key.value, &store.value));
  ok(syrnck_store_use_native_signer(store.value, "Vault", "vault@example.invalid", 0, signer.value, true));
  ok(syrnck_secret_key_close(key.value)); key.value = nullptr;
  ok(syrnck_signing_key_close(signer.value)); signer.value = nullptr;
  char id[65]{};
  ok(syrnck_object_put(store.value, "chunk", message.data(), message.size(), id));
  Bytes result(message.size()); std::size_t count = 0;
  ok(syrnck_object_read(store.value, "chunk", id, result.data(), result.size(), &count));
  require(result == message && count == message.size(), "vault-backed C Store changed data");
  const auto draft = Json{{"version", 1}, {"parents", Json::array()}, {"message", "vault signer ABI"},
                          {"timestamp", 9}, {"files", Json::object()}}.dump();
  ok(syrnck_prepare_native_revision_ex(store.value, reinterpret_cast<const unsigned char *>(draft.data()), draft.size(), nullptr, id));
  ok(syrnck_store_close(store.value)); store.value = nullptr;
  const auto envelope = read_bytes(root / "1.envelope", 104);
  Secret restored;
  ok(syrnck_secret_key_open(envelope.data(), envelope.size(), password.data(), password.size(), &restored.value));
  ok(syrnck_store_open_secret(path.c_str(), false, restored.value, &store.value));
  Bytes revision(64 * 1024); count = 0;
  ok(syrnck_object_read(store.value, "revision", id, revision.data(), revision.size(), &count));
  const auto signed_value = Json::parse(revision.begin(), revision.begin() + static_cast<std::ptrdiff_t>(count));
  require(signed_value.at("version") == 2 && signed_value.contains("signature"), "vault signer did not sign a revision");
  require(metadata(master) == realm, "provider close invalidated sealed owner");
}
void erase(Vault &vault, const fs::path &root) {
  const auto entries = refs(root);
  Signer signer; Master master; Secret key;
  ok(syrnck_system_vault_open_secret(vault.value, entries[0].c_str(), &key.value));
  ok(syrnck_system_vault_open_signer(vault.value, entries[1].c_str(), &signer.value));
  ok(syrnck_system_vault_open_sealed(vault.value, entries[2].c_str(), &master.value));
  const auto expected_signer = metadata(signer), expected_realm = metadata(master);
  for (const auto &ref : entries) { remove(vault, ref, true); remove(vault, ref, false); }
  require(metadata(signer) == expected_signer && metadata(master) == expected_realm, "erasure invalidated C key owners");
  CStore store;
  const auto path = utf8(root / "store");
  ok(syrnck_store_open_secret(path.c_str(), false, key.value, &store.value));
}
}
int main(int argc, char **argv) {
  try {
    if (argc == 1) { basic(); std::cout << "C vault ownership/output checks passed\n"; return 0; }
    require(argc == 3 || argc == 4, "unexpected ABI vault fixture arguments");
    const std::string mode(argv[1]); const auto root = path_from_utf8(argv[2]);
    Vault vault(argc == 4 ? argv[3] : nullptr);
    if (mode != "policy-required") ok(syrnck_system_vault_disable_macos_interaction());
    if (mode == "create") create(vault, root);
    else if (mode == "verify") verify(vault, root);
    else if (mode == "erase") erase(vault, root);
    else if (mode == "cleanup") {
      for (const auto &ref : refs(root)) { bool removed = false; ok(syrnck_system_vault_erase(vault.value, ref.c_str(), &removed)); }
    } else if (mode == "inaccessible" || mode == "policy-required") {
#ifdef __APPLE__
      if (mode == "policy-required") {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        require(SecKeychainSetUserInteractionAllowed(true) == errSecSuccess, "cannot set ABI fixture process policy");
#pragma clang diagnostic pop
      }
#endif
      for (const auto &ref : refs(root)) {
        Bytes out(104); std::size_t size = 0;
        require(syrnck_system_vault_export_protected(vault.value, ref.c_str(), password.data(), password.size(), nullptr,
            out.data(), out.size(), &size) != 0 && !size, "ABI vault did not refuse inaccessible entry");
        if (mode == "policy-required") require(std::string_view(syrnck_last_error()) == "macOS file key vault requires explicit noninteractive process setup",
            "ABI vault did not enforce explicit process policy");
      }
    } else throw Error("unexpected ABI vault fixture mode");
    std::cout << mode << " ABI checks passed\n";
    return 0;
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
