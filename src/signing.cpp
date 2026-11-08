#include "internal.hpp"
#include <syrnck/signing.hpp>
#include "signing_validation.hpp"
#include <monocypher.h>
#include <monocypher-ed25519.h>
#include <algorithm>
namespace syrnck {
namespace {
bool canonical_point(std::span<const std::uint8_t> point) noexcept {
  // Encoded y must be smaller than 2^255-19; the high bit is x's sign.
  bool smaller = false;
  for (int index = 31; index >= 0; --index) {
    const auto value = static_cast<unsigned>(point[static_cast<std::size_t>(index)] & (index == 31 ? 127U : 255U));
    const auto maximum = index == 31 ? 127U : index == 0 ? 237U : 255U;
    if (value < maximum) { smaller = true; break; }
    if (value > maximum) return false;
  }
  if (!smaller) return false;
  // RFC 8032 also rejects x=0 with the negative sign bit (y=1 or y=-1).
  if (point[31] & 128U) {
    bool positive_one = point[0] == 1, negative_one = point[0] == 236;
    for (std::size_t index = 1; index < 32; ++index) {
      const auto value = point[index] & (index == 31 ? 127U : 255U);
      positive_one = positive_one && value == 0;
      negative_one = negative_one && value == (index == 31 ? 127U : 255U);
    }
    if (positive_one || negative_one) return false;
  }
  return true;
}
bool small_order(std::span<const std::uint8_t> key) noexcept {
  std::array<std::uint8_t, 64> identity{}; identity[0] = 1;
  std::array<std::uint8_t, 32> one{}; one[0] = 1;
  return crypto_eddsa_check_equation(identity.data(), key.data(), one.data()) == 0;
}
}
bool valid_ed25519_public_key(std::span<const std::uint8_t> key) noexcept {
  if (key.size() != 32 || !canonical_point(key)) return false;
  std::array<std::uint8_t, 64> identity{}; identity[0] = 1;
  const std::array<std::uint8_t, 32> zero{};
  // With S=h=0 the upstream equation succeeds for any decodable curve point.
  return crypto_eddsa_check_equation(identity.data(), key.data(), zero.data()) == 0 && !small_order(key);
}
SigningKey SigningKey::from_seed(std::span<const std::uint8_t> input) {
  require(input.size() == 32, "signing seed must contain exactly 32 bytes");
  std::array<std::uint8_t, 32> seed;
  std::copy(input.begin(), input.end(), seed.begin());
  SigningKey result;
  // The upstream key-pair function wipes this mutable seed copy.
  crypto_ed25519_key_pair(result.secret_.data(), result.public_.data(), seed.data());
  result.valid_ = true;
  return result;
}
SigningKey SigningKey::generate() {
  auto seed = random_bytes(32);
  struct Wipe { Bytes &seed; ~Wipe() { crypto_wipe(seed.data(), seed.size()); } } wipe{seed};
  return from_seed(seed);
}
SigningKey::SigningKey(SigningKey &&other) noexcept
    : secret_(other.secret_), public_(other.public_), valid_(other.valid_) {
  crypto_wipe(other.secret_.data(), other.secret_.size()); other.valid_ = false;
}
SigningKey &SigningKey::operator=(SigningKey &&other) noexcept {
  if (this != &other) {
    crypto_wipe(secret_.data(), secret_.size());
    secret_ = other.secret_; public_ = other.public_; valid_ = other.valid_;
    crypto_wipe(other.secret_.data(), other.secret_.size()); other.valid_ = false;
  }
  return *this;
}
SigningKey::~SigningKey() { crypto_wipe(secret_.data(), secret_.size()); }
SigningPublicKey SigningKey::public_key() const { require(valid_, "signing key has been moved"); return public_; }
Ed25519Signature SigningKey::sign(std::span<const std::uint8_t> message) const {
  require(valid_, "signing key has been moved");
  Ed25519Signature result;
  crypto_ed25519_sign(result.data(), secret_.data(), message.data(), message.size());
  return result;
}
bool verify_ed25519(std::span<const std::uint8_t> key, std::span<const std::uint8_t> signature,
    std::span<const std::uint8_t> message) noexcept {
  if (key.size() != 32 || signature.size() != 64 || !canonical_point(key) || !canonical_point(signature.first(32))) return false;
  // Upstream checks the cofactored equation. R=identity, S=0 and h=1 make
  // that equation succeed exactly for small-order A. Such public keys
  // cannot establish possession and are refused for authentication.
  if (small_order(key)) return false;
  return crypto_ed25519_check(signature.data(), key.data(), message.data(), message.size()) == 0;
}
std::string signing_key_id(std::span<const std::uint8_t> public_key) {
  require(public_key.size() == 32, "signing public key must contain exactly 32 bytes");
  std::string value = "Syrnck Ed25519 public key v1:";
  value.append(reinterpret_cast<const char *>(public_key.data()), public_key.size());
  return "ed25519:" + hash(bytes(value));
}
} // namespace syrnck
