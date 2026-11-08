#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
namespace syrnck {
using SigningPublicKey = std::array<std::uint8_t, 32>;
using Ed25519Signature = std::array<std::uint8_t, 64>;
// Ed25519 (SHA-512, RFC 8032). Move-only secret ownership; moving invalidates
// the source and destruction wipes its expanded secret. No secret export API.
class SigningKey {
public:
  static SigningKey generate();
  static SigningKey from_seed(std::span<const std::uint8_t>);
  SigningKey(const SigningKey &) = delete;
  SigningKey &operator=(const SigningKey &) = delete;
  SigningKey(SigningKey &&) noexcept;
  SigningKey &operator=(SigningKey &&) noexcept;
  ~SigningKey();
  SigningPublicKey public_key() const;
  Ed25519Signature sign(std::span<const std::uint8_t> message) const;
private:
  SigningKey() = default;
  std::array<std::uint8_t, 64> secret_{};
  SigningPublicKey public_{};
  bool valid_ = false;
};
// Malformed lengths, noncanonical points and small-order public keys fail.
// Verification establishes signature validity; the caller authorizes the key.
bool verify_ed25519(std::span<const std::uint8_t> public_key,
    std::span<const std::uint8_t> signature, std::span<const std::uint8_t> message) noexcept;
std::string signing_key_id(std::span<const std::uint8_t> public_key);
} // namespace syrnck
