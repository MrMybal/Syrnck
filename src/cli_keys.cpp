#include "cli_keys.hpp"
#include "native_workspace.hpp"
#include <syrnck/keys.hpp>
#include <syrnck/key_vault.hpp>
#include <syrnck/signing_keys.hpp>
#include <syrnck/sealed.hpp>
#include <algorithm>
#include <cstdlib>
#include <mutex>
#ifdef _WIN32
#include <Windows.h>
#endif
namespace syrnck::cli {
namespace {
struct WipeBytes { Bytes &value; bool active = true; ~WipeBytes() { if (active) wipe_secret(value); } };
void environment_name(std::string_view name) {
  require(!name.empty() && name.size() <= 128 &&
      ((name.front() >= 'A' && name.front() <= 'Z') || name.front() == '_') &&
      std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
      }), "key environment name must use uppercase ASCII letters, digits and underscores");
}
std::optional<Bytes> environment_value(const std::string &name, std::size_t limit) {
  environment_name(name);
#ifdef _WIN32
  const std::wstring wide_name(name.begin(), name.end());
  SetLastError(ERROR_SUCCESS);
  const auto required = GetEnvironmentVariableW(wide_name.c_str(), nullptr, 0);
  if (!required) {
    if (GetLastError() == ERROR_ENVVAR_NOT_FOUND) return {};
    require(GetLastError() == ERROR_SUCCESS, "cannot read key environment");
    return Bytes{};
  }
  require(required <= limit + 1, "key environment value exceeds its limit");
  std::vector<wchar_t> wide(required);
  struct WipeWide { std::vector<wchar_t> &value; ~WipeWide() {
    wipe_secret({reinterpret_cast<std::uint8_t *>(value.data()), value.size() * sizeof(wchar_t)});
  } } wipe_wide{wide};
  SetLastError(ERROR_SUCCESS);
  const auto length = GetEnvironmentVariableW(wide_name.c_str(), wide.data(), required);
  require(length < required && (length || GetLastError() == ERROR_SUCCESS), "cannot read key environment");
  if (!length) return Bytes{};
  const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(length), nullptr, 0, nullptr, nullptr);
  require(size > 0 && static_cast<std::size_t>(size) <= limit, "key environment value exceeds its limit");
  Bytes result(static_cast<std::size_t>(size));
  WipeBytes wipe{result};
  require(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(length),
      reinterpret_cast<char *>(result.data()), size, nullptr, nullptr) == size, "cannot read key environment");
  // Transfer success to Environment's wiping owner. Wiping a named return
  // value here would also wipe the caller's bytes when NRVO is enabled.
  wipe.active = false;
  return result;
#else
  const auto value = std::getenv(name.c_str());
  if (!value) return {};
  const auto length = std::char_traits<char>::length(value);
  require(length <= limit, "key environment value exceeds its limit");
  return Bytes(value, value + length);
#endif
}
struct Environment {
  std::optional<Bytes> value;
  Environment(const std::string &name, std::size_t limit) : value(environment_value(name, limit)) {}
  Environment(const Environment &) = delete;
  Environment &operator=(const Environment &) = delete;
  ~Environment() { if (value) wipe_secret(*value); }
};
std::optional<SecretKey> raw_key(bool signing = false, bool sealed = false) {
  Environment raw(sealed ? "SYRNCK_SEALED_KEY" : signing ? "SYRNCK_SIGNING_SEED" : "SYRNCK_KEY", 64);
  if (!raw.value) return {};
  if (sealed) require(raw.value->size() == 64, "SYRNCK_SEALED_KEY must encode exactly 32 bytes");
  else if (signing) require(raw.value->size() == 64, "SYRNCK_SIGNING_SEED must encode exactly 32 bytes");
  else require(raw.value->size() == 64, "SYRNCK_KEY must encode exactly 32 bytes");
  auto decoded = unhex({reinterpret_cast<const char *>(raw.value->data()), raw.value->size()});
  WipeBytes wipe{decoded};
  return SecretKey::from_bytes(decoded);
}
Bytes key_file(const fs::path &path, bool signing = false, bool sealed = false) {
  const auto status = fs::symlink_status(path);
  require(fs::is_regular_file(status) && !native_link_like(path, status), "protected key file must be a regular non-linked file");
  auto value = read_bytes(path, 104);
  if (sealed) (void)protected_sealed_key_options(value);
  else if (signing) (void)protected_signing_key_options(value);
  else (void)protected_key_options(value);
  return value;
}
fs::path output_path(const std::string &name) {
  const auto supplied = fs::absolute(path_from_utf8(name)).lexically_normal();
  require(!name.empty() && !supplied.filename().empty(), "invalid protected key output path");
  const auto parent = fs::canonical(supplied.parent_path());
  require(fs::is_directory(parent), "protected key output parent must exist");
  const auto result = parent / supplied.filename();
  require(fs::symlink_status(result).type() == fs::file_type::not_found, "protected key output already exists");
  return result;
}
Json metadata(PasswordKdfOptions options, bool signing = false, bool sealed = false) {
  return Json{{"version",1},{"type",sealed ? "protected-sealed-key" : signing ? "protected-signing-key" : "protected-key"},{"bytes",104},{"kdf","argon2id"},
      {"memory_kib",options.memory_kib},{"passes",options.passes},{"lanes",1},
      {"cipher","xchacha20-poly1305"},{"authenticated",false}};
}
VaultKeyPurpose vault_purpose(std::string_view value) {
  if (value == "storage") return VaultKeyPurpose::storage;
  if (value == "signing") return VaultKeyPurpose::signing;
  if (value == "sealed") return VaultKeyPurpose::sealed;
  throw Error("vault purpose must be storage, signing or sealed");
}
void prepare_vault_cli() {
  // This executable is a headless host. Configure its process policy before
  // Keychain workers, only when an OS vault is actually selected.
  static std::once_flag prepared;
  std::call_once(prepared, [] { SystemKeyVault::disable_macos_interaction(); });
}
std::optional<fs::path> vault_keychain_environment() {
  Environment path("SYRNCK_VAULT_KEYCHAIN", 32768);
  if (!path.value) return {};
  require(!path.value->empty(), "SYRNCK_VAULT_KEYCHAIN must name an absolute macOS keychain");
  return path_from_utf8(text(*path.value));
}
VaultKeyReference environment_reference(const Bytes &value, VaultKeyPurpose purpose) {
  const auto ref = VaultKeyReference::parse(text(value));
  require(ref.purpose == purpose, "vault environment reference has another key purpose");
  return ref;
}
SecretKey vault_storage_environment(const Bytes &value) {
  const auto ref = environment_reference(value, VaultKeyPurpose::storage);
  const auto keychain = vault_keychain_environment();
  const SystemKeyVault vault(keychain);
  prepare_vault_cli();
  struct Cached { std::string reference; std::optional<fs::path> keychain; SecretKey secret; };
  struct Cache { std::mutex mutex; std::optional<Cached> entry; };
  static Cache cache;
  const std::lock_guard lock(cache.mutex);
  const auto encoded = ref.encode();
  if (!cache.entry || cache.entry->reference != encoded || cache.entry->keychain != keychain)
    cache.entry.emplace(Cached{encoded, keychain, vault.open_secret(ref)});
  // The immutable reference selects a retained native owner. Removing the OS
  // entry does not revoke it; restart the process to discard cached owners.
  return SecretKey::from_bytes(cache.entry->secret.view());
}
SigningKey vault_signing_environment(const Bytes &value) {
  const auto ref = environment_reference(value, VaultKeyPurpose::signing);
  const auto keychain = vault_keychain_environment();
  const SystemKeyVault vault(keychain);
  prepare_vault_cli();
  struct Cached { std::string reference; std::optional<fs::path> keychain; SigningKey key; };
  struct Cache { std::mutex mutex; std::optional<Cached> entry; };
  static Cache cache;
  const std::lock_guard lock(cache.mutex);
  const auto encoded = ref.encode();
  if (!cache.entry || cache.entry->reference != encoded || cache.entry->keychain != keychain)
    cache.entry.emplace(Cached{encoded, keychain, vault.open_signer(ref)});
  return cache.entry->key.clone();
}
struct VaultReceipt { VaultKeyReference reference; std::optional<fs::path> keychain; };
VaultReceipt vault_receipt(const std::string &input) {
  const auto path = path_from_utf8(input);
  const auto status = fs::symlink_status(path);
  require(fs::is_regular_file(status) && !native_link_like(path, status), "vault reference must be a regular non-linked file");
  const auto value = parse_unique_json(read_bytes(path, 16384));
  require(value.is_object() && (value.size() == 4 || value.size() == 5) &&
      value.contains("version") && value.at("version").is_number_integer() && value.at("version") == 1 &&
      value.contains("type") && value.at("type").is_string() && value.at("type") == "system-vault-reference" &&
      value.contains("provider") && value.at("provider").is_string() &&
      value.contains("reference") && value.at("reference").is_string() &&
      (value.size() == 4 || (value.contains("keychain") && value.at("keychain").is_string())), "invalid vault reference file");
  require(value.at("provider").get<std::string>() == SystemKeyVault::provider(), "vault reference belongs to another system provider");
  std::optional<fs::path> keychain;
  if (value.contains("keychain")) keychain = path_from_utf8(value.at("keychain").get<std::string>());
  return {VaultKeyReference::parse(value.at("reference").get<std::string>()), std::move(keychain)};
}
fs::path vault_reference_output(const std::string &name) {
  require(!name.empty(), "invalid vault reference output path");
  const auto supplied = fs::absolute(path_from_utf8(name)).lexically_normal();
  require(!supplied.filename().empty(), "invalid vault reference output path");
  const auto parent = fs::canonical(supplied.parent_path());
  require(fs::is_directory(parent), "vault reference output parent must exist");
  const auto output = parent / supplied.filename();
  require(fs::symlink_status(output).type() == fs::file_type::not_found, "vault reference output already exists");
  return output;
}
}
std::optional<Key> encryption_key_environment() {
  Environment file("SYRNCK_KEY_FILE", 32768);
  Environment reference("SYRNCK_KEY_VAULT", 96);
  if (reference.value) {
    Environment raw("SYRNCK_KEY", 64);
    require(!raw.value && !file.value, "choose only one of SYRNCK_KEY, SYRNCK_KEY_FILE or SYRNCK_KEY_VAULT");
    const auto owner = vault_storage_environment(*reference.value);
    Key result; std::copy(owner.view().begin(), owner.view().end(), result.begin());
    return result;
  }
  if (!file.value) {
    auto secret = raw_key();
    if (!secret) return {};
    Key result;
    std::copy(secret->view().begin(), secret->view().end(), result.begin());
    return result;
  }
  Environment raw("SYRNCK_KEY", 64);
  require(!raw.value, "choose either SYRNCK_KEY or SYRNCK_KEY_FILE");
  require(!file.value->empty(), "SYRNCK_KEY_FILE must name a protected key file");
  Environment password("SYRNCK_KEY_PASSWORD", 4096);
  require(password.value && !password.value->empty(), "set SYRNCK_KEY_PASSWORD for the protected key file");
  const auto envelope = key_file(path_from_utf8(text(*file.value)));
  // Resident agents reopen short Store sessions. Reuse only a fully
  // authenticated key for the exact envelope AND password; never trust stamps.
  // A failed new authentication preserves the previous entry. No password or
  // derived fingerprint is logged or persisted.
  auto identity = envelope;
  WipeBytes wipe_identity{identity};
  identity.insert(identity.end(), password.value->begin(), password.value->end());
  const auto fingerprint = hash(identity);
  struct CachedKey { std::string fingerprint; SecretKey secret; };
  struct Cache { std::mutex mutex; std::optional<CachedKey> entry; };
  static Cache cache;
  const std::lock_guard lock(cache.mutex);
  if (!cache.entry || cache.entry->fingerprint != fingerprint) {
    CachedKey next{fingerprint, open_secret_key(envelope, *password.value)};
    cache.entry.emplace(std::move(next));
  }
  Key result;
  std::copy(cache.entry->secret.view().begin(), cache.entry->secret.view().end(), result.begin());
  return result;
}
std::optional<SigningKey> protected_signing_key_environment() {
  Environment file("SYRNCK_SIGNING_KEY_FILE", 32768);
  Environment reference("SYRNCK_SIGNING_KEY_VAULT", 96);
  if (reference.value) {
    Environment raw("SYRNCK_SIGNING_SEED", 64);
    require(!raw.value && !file.value, "choose only one of SYRNCK_SIGNING_SEED, SYRNCK_SIGNING_KEY_FILE or SYRNCK_SIGNING_KEY_VAULT");
    return vault_signing_environment(*reference.value);
  }
  if (!file.value) return {};
  Environment raw("SYRNCK_SIGNING_SEED", 64);
  require(!raw.value, "choose either SYRNCK_SIGNING_SEED or SYRNCK_SIGNING_KEY_FILE");
  require(!file.value->empty(), "SYRNCK_SIGNING_KEY_FILE must name a protected signing key file");
  Environment password("SYRNCK_SIGNING_KEY_PASSWORD", 4096);
  require(password.value && !password.value->empty(), "set SYRNCK_SIGNING_KEY_PASSWORD for the protected signing key file");
  const auto envelope = key_file(path_from_utf8(text(*file.value)), true);
  return open_protected_signing_key(envelope, *password.value);
}
void discard_sealed_key_environment() {
  for (const auto *name : {"SYRNCK_SEALED_KEY", "SYRNCK_SEALED_KEY_FILE", "SYRNCK_SEALED_KEY_VAULT", "SYRNCK_SEALED_KEY_PASSWORD", "SYRNCK_NEW_SEALED_KEY_PASSWORD"}) {
#ifdef _WIN32
    const std::wstring wide(name, name + std::char_traits<char>::length(name));
    SetLastError(ERROR_SUCCESS);
    const auto result = SetEnvironmentVariableW(wide.c_str(), nullptr);
    require((result != 0 || GetLastError() == ERROR_ENVVAR_NOT_FOUND) && _putenv_s(name, "") == 0,
            "cannot clear sealed key environment");
#else
    require(::unsetenv(name) == 0, "cannot clear sealed key environment");
#endif
  }
}
std::optional<SecretKey> sealed_key_environment() {
  const auto read = []() -> std::optional<SecretKey> {
    Environment file("SYRNCK_SEALED_KEY_FILE", 32768);
    Environment reference("SYRNCK_SEALED_KEY_VAULT", 96);
    if (reference.value) {
      Environment raw("SYRNCK_SEALED_KEY", 64);
      require(!raw.value && !file.value, "choose only one of SYRNCK_SEALED_KEY, SYRNCK_SEALED_KEY_FILE or SYRNCK_SEALED_KEY_VAULT");
      const auto ref = environment_reference(*reference.value, VaultKeyPurpose::sealed);
      const SystemKeyVault vault(vault_keychain_environment());
      prepare_vault_cli();
      return vault.open_secret(ref);
    }
    if (!file.value) return raw_key(false, true);
    Environment raw("SYRNCK_SEALED_KEY", 64);
    require(!raw.value, "choose either SYRNCK_SEALED_KEY or SYRNCK_SEALED_KEY_FILE");
    require(!file.value->empty(), "SYRNCK_SEALED_KEY_FILE must name a protected sealed key file");
    Environment password("SYRNCK_SEALED_KEY_PASSWORD", 4096);
    require(password.value && !password.value->empty(), "set SYRNCK_SEALED_KEY_PASSWORD for the protected sealed key file");
    return open_protected_sealed_key(key_file(path_from_utf8(text(*file.value)), false, true), *password.value);
  };
  try { auto key = read(); discard_sealed_key_environment(); return key; }
  catch (...) { discard_sealed_key_environment(); throw; }
}
Json run_vault_command(const VaultCommand &request) {
  if (request.action == "provider")
    return Json{{"version", 1}, {"provider", SystemKeyVault::provider()}, {"available", SystemKeyVault::provider() != "unavailable"}};
  if (request.action == "create" || request.action == "import") {
    const auto purpose = vault_purpose(request.purpose);
    const auto output = vault_reference_output(request.output);
    std::optional<fs::path> keychain;
    if (request.keychain) keychain = path_from_utf8(*request.keychain);
    const SystemKeyVault vault(keychain);
    Bytes envelope;
    std::optional<Environment> password;
    if (request.action == "import") {
      envelope = key_file(path_from_utf8(request.input), purpose == VaultKeyPurpose::signing, purpose == VaultKeyPurpose::sealed);
      password.emplace(request.password_env, 4096);
      require(password->value && !password->value->empty(), "set the selected key password environment variable");
    }
    prepare_vault_cli();
    const auto ref = request.action == "create" ? vault.generate(purpose) : vault.import_protected(envelope, *password->value, purpose);
    Json receipt;
    try {
      receipt = Json{{"version", 1}, {"type", "system-vault-reference"}, {"provider", SystemKeyVault::provider()}, {"reference", ref.encode()}};
      if (keychain) receipt["keychain"] = utf8(*keychain);
      atomic_write(output, bytes(receipt.dump()), false);
    }
    catch (const std::exception &error) {
      // OS and filesystem publication are separate. Never revoke a key whose
      // reference file may already be visible after a durability error.
      const auto detail = ref.encode() + "; " + error.what();
      throw Error("vault entry retained after reference publication error: " + detail);
    }
    receipt["path"] = utf8(output);
    return receipt;
  }
  require(request.action == "inspect" || request.action == "export" || request.action == "erase", "unknown vault command; use provider, create, import, inspect, export or erase");
  const auto receipt = vault_receipt(request.input);
  const SystemKeyVault vault(receipt.keychain);
  std::optional<fs::path> output;
  PasswordKdfOptions options;
  std::optional<Environment> password;
  if (request.action == "export") {
    output = output_path(request.output);
    if (request.memory_mib) {
      require(*request.memory_mib >= 64 && *request.memory_mib <= 256, "password KDF memory must be 64..256 whole MiB");
      options.memory_kib = *request.memory_mib * 1024;
    }
    if (request.passes) options.passes = *request.passes;
    options.validate();
    password.emplace(request.password_env, 4096);
    require(password->value && !password->value->empty(), "set the selected key password environment variable");
  }
  prepare_vault_cli();
  Json result{{"version", 1}, {"provider", SystemKeyVault::provider()}, {"reference", receipt.reference.encode()}};
  if (request.action == "erase") { result["removed"] = vault.erase(receipt.reference); return result; }
  if (request.action == "export") {
    const auto envelope = vault.export_protected(receipt.reference, *password->value, options);
    atomic_write(*output, envelope, false);
    result["path"] = utf8(*output); result["bytes"] = envelope.size();
    return result;
  }
  if (receipt.reference.purpose == VaultKeyPurpose::signing) {
    const auto signer = vault.open_signer(receipt.reference);
    result["public_key"] = hex(signer.public_key()); result["key_id"] = signing_key_id(signer.public_key());
  } else {
    const auto key = vault.open_secret(receipt.reference);
    if (receipt.reference.purpose == VaultKeyPurpose::sealed) result["realm"] = sealed_realm_id(key);
  }
  result["authenticated"] = true;
  return result;
}
Json run_key_command(const KeyCommand &request) {
  require(!request.signing || !request.sealed, "invalid protected key purpose");
  require(request.action == "create" || request.action == "protect" || request.action == "rewrap" ||
      request.action == "inspect" || request.action == "verify",
      "unknown key command; use create, protect, rewrap, inspect or verify");
  if (request.action == "inspect" || request.action == "verify") {
    const auto envelope = key_file(path_from_utf8(request.input), request.signing, request.sealed);
    auto result = metadata(request.sealed ? protected_sealed_key_options(envelope) : request.signing ? protected_signing_key_options(envelope) : protected_key_options(envelope), request.signing, request.sealed);
    if (request.action == "verify") {
      Environment password(request.password_env, 4096);
      require(password.value && !password.value->empty(), "set the selected key password environment variable");
      if (request.signing) {
        auto signer = open_protected_signing_key(envelope, *password.value);
        result["public_key"] = hex(signer.public_key());
        result["key_id"] = signing_key_id(signer.public_key());
      } else if (request.sealed) result["realm"] = sealed_realm_id(open_protected_sealed_key(envelope, *password.value));
      else (void)open_secret_key(envelope, *password.value);
      result["authenticated"] = true;
    }
    return result;
  }
  const auto output = output_path(request.output);
  Bytes input;
  auto options = PasswordKdfOptions{};
  if (request.action == "rewrap") {
    input = key_file(path_from_utf8(request.input), request.signing, request.sealed);
    options = request.sealed ? protected_sealed_key_options(input) : request.signing ? protected_signing_key_options(input) : protected_key_options(input);
  }
  if (request.memory_mib) {
    require(*request.memory_mib >= 64 && *request.memory_mib <= 256, "password KDF memory must be 64..256 whole MiB");
    options.memory_kib = *request.memory_mib * 1024;
  }
  if (request.passes) options.passes = *request.passes;
  options.validate();
  Environment password(request.password_env, 4096);
  require(password.value && !password.value->empty(), "set the selected key password environment variable");
  Bytes envelope;
  std::optional<SigningPublicKey> public_key;
  if (request.action == "rewrap") {
    Environment next(request.new_password_env, 4096);
    require(next.value && !next.value->empty(), "set the selected new key password environment variable");
    if (request.signing) {
      auto sealed = change_signing_key_password(input, *password.value, *next.value, options);
      envelope = std::move(sealed.envelope); public_key = sealed.public_key;
    } else if (request.sealed) envelope = change_sealed_key_password(input, *password.value, *next.value, options);
    else envelope = change_secret_key_password(input, *password.value, *next.value, options);
  } else {
    auto secret = request.action == "create" ? std::optional{SecretKey::generate()} : raw_key(request.signing, request.sealed);
    if (request.sealed) require(secret.has_value(), "set SYRNCK_SEALED_KEY before protecting an existing sealed key");
    else if (request.signing) require(secret.has_value(), "set SYRNCK_SIGNING_SEED before protecting an existing signer");
    else require(secret.has_value(), "set SYRNCK_KEY before protecting an existing key");
    if (request.signing) {
      auto sealed = protect_signing_seed(secret->view(), *password.value, options);
      envelope = std::move(sealed.envelope); public_key = sealed.public_key;
    } else if (request.sealed) envelope = protect_sealed_key(*secret, *password.value, options);
    else envelope = protect_secret_key(*secret, *password.value, options);
  }
  atomic_write(output, envelope, false);
  auto result = metadata(options, request.signing, request.sealed);
  if (public_key) {
    result["public_key"] = hex(*public_key);
    result["key_id"] = signing_key_id(*public_key);
  }
  result["authenticated"] = true;
  result["path"] = utf8(output);
  return result;
}
} // namespace syrnck::cli
