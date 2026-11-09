#include <syrnck/keys.hpp>
#include "key_envelope.hpp"
#include <monocypher.h>
#include <algorithm>
#include <vector>

namespace syrnck {
namespace {
constexpr std::array<std::uint8_t, 8> key_magic{'S','Y','R','K','E','Y',1,0};
constexpr std::array<std::uint8_t, 8> signing_magic{'S','Y','R','S','I','G',1,0};
const std::array<std::uint8_t, 8> &purpose_magic(detail::SecretPurpose purpose) {
  switch (purpose) {
  case detail::SecretPurpose::storage_key: return key_magic;
  case detail::SecretPurpose::signing_seed: return signing_magic;
  }
  throw Error("invalid protected key purpose");
}
constexpr std::size_t header_size = 56, envelope_size = 104;
void password_size(std::span<const std::uint8_t> password) {
  require(!password.empty() && password.size() <= 4096, "key password must contain 1..4096 bytes");
}
struct Wipe {
  std::span<std::uint8_t> value;
  ~Wipe() { wipe_secret(value); }
};
void number(std::span<std::uint8_t> output, std::uint32_t value) {
  for (unsigned at = 0; at < 4; ++at) output[at] = static_cast<std::uint8_t>(value >> (8 * at));
}
std::uint32_t number(std::span<const std::uint8_t> input) {
  std::uint32_t result = 0;
  for (unsigned at = 0; at < 4; ++at) result |= std::uint32_t(input[at]) << (8 * at);
  return result;
}
}
void PasswordKdfOptions::validate() const {
  require(memory_kib >= 64 * 1024 && memory_kib <= 256 * 1024 && memory_kib % 1024 == 0,
      "password KDF memory must be 64..256 whole MiB");
  require(passes >= 3 && passes <= 10, "password KDF passes must be in 3..10");
}
void wipe_secret(std::span<std::uint8_t> value) noexcept { crypto_wipe(value.data(), value.size()); }
SecretKey SecretKey::from_bytes(std::span<const std::uint8_t> value) {
  require(value.size() == 32, "secret key must contain exactly 32 bytes");
  SecretKey result;
  std::copy(value.begin(), value.end(), result.value_.begin());
  result.valid_ = true;
  return result;
}
SecretKey SecretKey::generate() {
  auto random = random_bytes(32);
  Wipe wipe{random};
  return from_bytes(random);
}
SecretKey SecretKey::from_password(std::span<const std::uint8_t> password,
    std::span<const std::uint8_t> salt, PasswordKdfOptions options) {
  options.validate(); password_size(password);
  require(salt.size() == 16, "password KDF salt must contain exactly 16 bytes");
  // uint64_t alignment is required by Monocypher. The validated upper bound
  // also prevents multiplication overflow on supported 32-bit platforms.
  std::vector<std::uint64_t> workspace(std::size_t(options.memory_kib) * 128);
  Wipe wipe{{reinterpret_cast<std::uint8_t *>(workspace.data()), workspace.size() * sizeof(std::uint64_t)}};
  SecretKey result;
  crypto_argon2(result.value_.data(), 32, workspace.data(),
      {CRYPTO_ARGON2_ID, options.memory_kib, options.passes, 1},
      {password.data(), salt.data(), static_cast<std::uint32_t>(password.size()), 16},
      crypto_argon2_no_extras);
  result.valid_ = true;
  return result;
}
SecretKey::SecretKey(SecretKey &&other) noexcept : value_(other.value_), valid_(other.valid_) {
  wipe_secret(other.value_); other.valid_ = false;
}
SecretKey &SecretKey::operator=(SecretKey &&other) noexcept {
  if (this != &other) {
    wipe_secret(value_); value_ = other.value_; valid_ = other.valid_;
    wipe_secret(other.value_); other.valid_ = false;
  }
  return *this;
}
SecretKey::~SecretKey() { wipe_secret(value_); }
std::span<const std::uint8_t, 32> SecretKey::view() const & {
  require(valid_, "secret key has been moved");
  return value_;
}
Bytes detail::protect_secret_for(const SecretKey &secret, std::span<const std::uint8_t> password,
    PasswordKdfOptions options, SecretPurpose purpose) {
  const auto &magic = purpose_magic(purpose);
  options.validate(); password_size(password);
  const auto plain = secret.view();
  Bytes result(envelope_size);
  std::copy(magic.begin(), magic.end(), result.begin());
  number(std::span(result).subspan(8, 4), options.memory_kib);
  number(std::span(result).subspan(12, 4), options.passes);
  const auto random = random_bytes(40); // Public salt (16) and nonce (24).
  std::copy(random.begin(), random.end(), result.begin() + 16);
  const auto key = SecretKey::from_password(password, std::span(result).subspan(16, 16), options);
  crypto_aead_lock(result.data() + header_size, result.data() + 88, key.view().data(),
      result.data() + 32, result.data(), header_size, plain.data(), plain.size());
  return result;
}
PasswordKdfOptions detail::protected_options_for(std::span<const std::uint8_t> envelope, SecretPurpose purpose) {
  const auto &magic = purpose_magic(purpose);
  require(envelope.size() == envelope_size && std::equal(magic.begin(), magic.end(), envelope.begin()),
      "invalid protected key envelope");
  const PasswordKdfOptions options{number(envelope.subspan(8, 4)), number(envelope.subspan(12, 4))};
  options.validate();
  return options;
}
SecretKey detail::open_secret_for(std::span<const std::uint8_t> envelope,
    std::span<const std::uint8_t> password, SecretPurpose purpose) {
  const auto options = protected_options_for(envelope, purpose);
  password_size(password);
  const auto key = SecretKey::from_password(password, envelope.subspan(16, 16), options);
  Key plain{}; Wipe wipe{plain};
  require(crypto_aead_unlock(plain.data(), envelope.data() + 88, key.view().data(),
      envelope.data() + 32, envelope.data(), header_size, envelope.data() + header_size, plain.size()) == 0,
      "protected key authentication failed");
  return SecretKey::from_bytes(plain);
}
Bytes protect_secret_key(const SecretKey &secret, std::span<const std::uint8_t> password,
    PasswordKdfOptions options) {
  return detail::protect_secret_for(secret, password, options, detail::SecretPurpose::storage_key);
}
PasswordKdfOptions protected_key_options(std::span<const std::uint8_t> envelope) {
  return detail::protected_options_for(envelope, detail::SecretPurpose::storage_key);
}
SecretKey open_secret_key(std::span<const std::uint8_t> envelope, std::span<const std::uint8_t> password) {
  return detail::open_secret_for(envelope, password, detail::SecretPurpose::storage_key);
}
Bytes change_secret_key_password(std::span<const std::uint8_t> envelope,
    std::span<const std::uint8_t> old_password, std::span<const std::uint8_t> new_password,
    std::optional<PasswordKdfOptions> options) {
  if (options) options->validate();
  password_size(new_password);
  const auto secret = open_secret_key(envelope, old_password);
  const auto costs = options.value_or(PasswordKdfOptions{
      number(envelope.subspan(8, 4)), number(envelope.subspan(12, 4))});
  return protect_secret_key(secret, new_password, costs);
}
} // namespace syrnck
