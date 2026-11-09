#include <syrnck/signing_keys.hpp>
#include "key_envelope.hpp"
namespace syrnck {
namespace {
ProtectedSigningKey protect(const SecretKey &seed, std::span<const std::uint8_t> password,
                            PasswordKdfOptions options) {
  auto signer = SigningKey::from_seed(seed.view());
  return {detail::protect_secret_for(seed, password, options, detail::SecretPurpose::signing_seed),
          signer.public_key()};
}
} // namespace
ProtectedSigningKey protect_signing_seed(std::span<const std::uint8_t> seed,
                                         std::span<const std::uint8_t> password, PasswordKdfOptions options) {
  require(seed.size() == 32, "signing seed must contain exactly 32 bytes");
  return protect(SecretKey::from_bytes(seed), password, options);
}
ProtectedSigningKey generate_protected_signing_key(std::span<const std::uint8_t> password,
                                                   PasswordKdfOptions options) {
  return protect(SecretKey::generate(), password, options);
}
PasswordKdfOptions protected_signing_key_options(std::span<const std::uint8_t> envelope) {
  return detail::protected_options_for(envelope, detail::SecretPurpose::signing_seed);
}
SigningKey open_protected_signing_key(std::span<const std::uint8_t> envelope,
                                      std::span<const std::uint8_t> password) {
  const auto seed = detail::open_secret_for(envelope, password, detail::SecretPurpose::signing_seed);
  return SigningKey::from_seed(seed.view());
}
ProtectedSigningKey change_signing_key_password(std::span<const std::uint8_t> envelope,
                                                std::span<const std::uint8_t> before,
                                                std::span<const std::uint8_t> after,
                                                std::optional<PasswordKdfOptions> options) {
  if (options)
    options->validate();
  require(!after.empty() && after.size() <= 4096, "key password must contain 1..4096 bytes");
  const auto costs = protected_signing_key_options(envelope);
  const auto seed = detail::open_secret_for(envelope, before, detail::SecretPurpose::signing_seed);
  return protect(seed, after, options.value_or(costs));
}
} // namespace syrnck
