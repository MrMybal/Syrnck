#include <syrnck/sealed.hpp>
#include "key_envelope.hpp"
namespace syrnck {
Bytes protect_sealed_key(const SecretKey &key, std::span<const std::uint8_t> password,
                         PasswordKdfOptions options) {
  return detail::protect_secret_for(key, password, options, detail::SecretPurpose::sealed_master);
}
PasswordKdfOptions protected_sealed_key_options(std::span<const std::uint8_t> envelope) {
  return detail::protected_options_for(envelope, detail::SecretPurpose::sealed_master);
}
SecretKey open_protected_sealed_key(std::span<const std::uint8_t> envelope,
                                    std::span<const std::uint8_t> password) {
  return detail::open_secret_for(envelope, password, detail::SecretPurpose::sealed_master);
}
Bytes change_sealed_key_password(std::span<const std::uint8_t> envelope, std::span<const std::uint8_t> before,
                                 std::span<const std::uint8_t> after,
                                 std::optional<PasswordKdfOptions> options) {
  if (options)
    options->validate();
  require(!after.empty() && after.size() <= 4096, "key password must contain 1..4096 bytes");
  const auto costs = protected_sealed_key_options(envelope);
  const auto secret = open_protected_sealed_key(envelope, before);
  return protect_sealed_key(secret, after, options.value_or(costs));
}
} // namespace syrnck
