#include <syrnck/key_vault.hpp>
#include "key_vault_backend.hpp"
#include "key_envelope.hpp"
#include <algorithm>

namespace syrnck {
namespace {
constexpr std::array<std::uint8_t, 8> magic{'S','Y','R','V','L','T',1,0};
std::string_view purpose_name(VaultKeyPurpose purpose) {
  switch (purpose) {
  case VaultKeyPurpose::storage: return "storage";
  case VaultKeyPurpose::signing: return "signing";
  case VaultKeyPurpose::sealed: return "sealed";
  }
  throw Error("invalid system key vault purpose");
}
detail::SecretPurpose envelope_purpose(VaultKeyPurpose purpose) {
  switch (purpose) {
  case VaultKeyPurpose::storage: return detail::SecretPurpose::storage_key;
  case VaultKeyPurpose::signing: return detail::SecretPurpose::signing_seed;
  case VaultKeyPurpose::sealed: return detail::SecretPurpose::sealed_master;
  }
  throw Error("invalid system key vault purpose");
}
struct Wipe { Bytes &value; ~Wipe() { wipe_secret(value); } };
void validate(const VaultKeyReference &ref) {
  (void)purpose_name(ref.purpose);
  require(ref.id.size() == 64 && std::all_of(ref.id.begin(), ref.id.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  }), "invalid system key vault reference");
}
} // namespace

VaultKeyReference VaultKeyReference::parse(std::string_view encoded) {
  constexpr std::string_view prefix = "syrnck-vault:1:";
  require(encoded.size() <= 96 && encoded.starts_with(prefix), "invalid system key vault reference");
  encoded.remove_prefix(prefix.size());
  const auto colon = encoded.find(':');
  require(colon != std::string_view::npos, "invalid system key vault reference");
  const auto name = encoded.substr(0, colon);
  VaultKeyPurpose purpose;
  if (name == "storage") purpose = VaultKeyPurpose::storage;
  else if (name == "signing") purpose = VaultKeyPurpose::signing;
  else if (name == "sealed") purpose = VaultKeyPurpose::sealed;
  else throw Error("invalid system key vault reference");
  VaultKeyReference result{purpose, std::string(encoded.substr(colon + 1))};
  validate(result);
  return result;
}
std::string VaultKeyReference::encode() const {
  validate(*this);
  return "syrnck-vault:1:" + std::string(purpose_name(purpose)) + ":" + id;
}
SystemKeyVault::SystemKeyVault(std::optional<fs::path> keychain) : keychain_(std::move(keychain)) {
  require(!keychain_ || (!keychain_->empty() && keychain_->is_absolute()),
          "system key vault keychain path must be absolute");
  require(!keychain_ || keychain_->native().find(fs::path::value_type{}) == fs::path::string_type::npos,
          "system key vault keychain path contains a null character");
#ifndef __APPLE__
  require(!keychain_, "a keychain path is supported only on macOS");
#endif
}
std::string_view SystemKeyVault::provider() noexcept { return detail::vault_provider(); }
void SystemKeyVault::disable_macos_interaction() { detail::vault_disable_macos_interaction(); }
VaultKeyReference SystemKeyVault::save(const SecretKey &secret, VaultKeyPurpose purpose) const {
  (void)purpose_name(purpose);
  const auto plain = secret.view();
  VaultKeyReference ref{purpose, hex(random_bytes(32))};
  auto existing = detail::vault_read(keychain_, ref);
  if (existing) { Wipe wipe{*existing}; throw Error("system key vault entry already exists"); }
  Bytes record; Wipe wipe{record};
  record.reserve(detail::vault_record_size);
  record.insert(record.end(), magic.begin(), magic.end());
  record.push_back(static_cast<std::uint8_t>(purpose));
  const auto id = unhex(ref.id);
  record.insert(record.end(), id.begin(), id.end());
  record.insert(record.end(), plain.begin(), plain.end());
  detail::vault_write(keychain_, ref, record);
  return ref;
}
VaultKeyReference SystemKeyVault::generate(VaultKeyPurpose purpose) const {
  return save(SecretKey::generate(), purpose);
}
VaultKeyReference SystemKeyVault::import_protected(std::span<const std::uint8_t> envelope,
    std::span<const std::uint8_t> password, VaultKeyPurpose purpose) const {
  const auto secret = detail::open_secret_for(envelope, password, envelope_purpose(purpose));
  return save(secret, purpose);
}
SecretKey detail::decode_vault_record(const VaultKeyReference &ref, std::span<const std::uint8_t> record) {
  validate(ref);
  const auto id = unhex(ref.id);
  require(record.size() == detail::vault_record_size &&
      std::equal(magic.begin(), magic.end(), record.begin()) &&
      record[8] == static_cast<std::uint8_t>(ref.purpose) &&
      std::equal(id.begin(), id.end(), record.begin() + 9), "invalid system key vault record");
  return SecretKey::from_bytes(record.subspan(41));
}
SecretKey SystemKeyVault::read(const VaultKeyReference &ref) const {
  validate(ref);
  auto result = detail::vault_read(keychain_, ref);
  require(result.has_value(), "system key vault entry was not found");
  Wipe wipe{*result};
  return detail::decode_vault_record(ref, *result);
}
SecretKey SystemKeyVault::open_secret(const VaultKeyReference &ref) const {
  require(ref.purpose == VaultKeyPurpose::storage || ref.purpose == VaultKeyPurpose::sealed,
          "system key vault reference is not a storage or sealed key");
  return read(ref);
}
SigningKey SystemKeyVault::open_signer(const VaultKeyReference &ref) const {
  require(ref.purpose == VaultKeyPurpose::signing, "system key vault reference is not a signing key");
  const auto seed = read(ref);
  return SigningKey::from_seed(seed.view());
}
Bytes SystemKeyVault::export_protected(const VaultKeyReference &ref, std::span<const std::uint8_t> password,
    PasswordKdfOptions options) const {
  options.validate();
  require(!password.empty() && password.size() <= 4096, "key password must contain 1..4096 bytes");
  const auto secret = read(ref);
  return detail::protect_secret_for(secret, password, options, envelope_purpose(ref.purpose));
}
bool SystemKeyVault::erase(const VaultKeyReference &ref) const {
  validate(ref);
  return detail::vault_erase(keychain_, ref);
}
} // namespace syrnck
