#include "native_history_fixture.hpp"
#include <syrnck/signing.hpp>
#include <monocypher.h>
#include <monocypher-ed25519.h>
#include <type_traits>
#include <iostream>
using namespace native_test;
static_assert(!std::is_copy_constructible_v<SigningKey> && !std::is_copy_assignable_v<SigningKey>);
static_assert(std::is_nothrow_move_constructible_v<SigningKey> && std::is_nothrow_move_assignable_v<SigningKey>);
namespace {
struct Vector { const char *seed, *key, *message, *signature; };
// Public RFC 8032 section 7.1 test seeds, never deployment keys.
constexpr Vector vectors[]{
  {"9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
   "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
   "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555f"
   "b8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"},
  {"4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
   "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
   "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
   "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"},
  {"c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
   "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
   "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18"
   "ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"}
};
void reference_vectors() {
  for (const auto &value : vectors) {
    const auto seed = unhex(value.seed), message = unhex(value.message);
    const auto before = seed;
    auto key = SigningKey::from_seed(seed);
    const auto public_key = key.public_key();
    const auto signature = key.sign(message);
    require(seed == before && hex(public_key) == value.key && hex(signature) == value.signature &&
        verify_ed25519(public_key, signature, message), "RFC 8032 signature vector mismatch");
    for (std::size_t byte = 0; byte < signature.size(); ++byte) for (unsigned bit = 0; bit < 8; ++bit) {
      auto altered = signature; altered[byte] ^= static_cast<std::uint8_t>(1U << bit);
      require(!verify_ed25519(public_key, altered, message), "tampered signature verified");
    }
    auto changed = message; changed.push_back(0);
    require(!verify_ed25519(public_key, signature, changed), "changed message verified");
    const auto identifier = signing_key_id(public_key);
    auto moved = std::move(key);
    fails([&] { key.sign(message); }); fails([&] { key.public_key(); });
    auto assigned = SigningKey::generate(); assigned = std::move(moved);
    fails([&] { moved.sign(message); });
    require(assigned.sign(message) == signature && signing_key_id(assigned.public_key()) == identifier,
        "moving a signing key changed its identity or signature");
    auto cloned = assigned.clone();
    assigned = SigningKey::generate();
    require(cloned.sign(message) == signature && signing_key_id(cloned.public_key()) == identifier,
        "independent native signing owner changed its identity");
    auto retained_clone = std::move(cloned);
    fails([&] { cloned.clone(); });
    require(retained_clone.sign(message) == signature, "moved native clone changed signature");
  }
}
void malformed() {
  auto signer = SigningKey::generate();
  const auto key = signer.public_key();
  const auto signature = signer.sign(bytes("Syrnck signature test"));
  for (std::size_t size = 0; size < key.size(); ++size)
    require(!verify_ed25519(std::span(key).first(size), signature, bytes("Syrnck signature test")), "short public key accepted");
  for (std::size_t size = 0; size < signature.size(); ++size)
    require(!verify_ed25519(key, std::span(signature).first(size), bytes("Syrnck signature test")), "short signature accepted");
  fails([&] { SigningKey::from_seed({}); }); fails([&] { signing_key_id({}); });
  Ed25519Signature forgery{}; forgery[0] = 1;
  for (unsigned sign : {0U, 128U}) for (unsigned choice : {0U, 1U, 2U}) {
    SigningPublicKey weak{};
    if (choice == 1) weak[0] = 1;
    if (choice == 2) { weak.fill(255); weak[0] = 236; weak[31] = 127; }
    weak[31] |= static_cast<std::uint8_t>(sign);
    require(!verify_ed25519(weak, forgery, bytes("arbitrary payload")), "small-order public key authenticated a forgery");
  }
  auto noncanonical = key; noncanonical.fill(255); noncanonical[0] = 237; noncanonical[31] = 127;
  require(!verify_ed25519(noncanonical, forgery, {}), "noncanonical public key accepted");
  auto bad_r = signature; std::copy(noncanonical.begin(), noncanonical.end(), bad_r.begin());
  require(!verify_ed25519(key, bad_r, bytes("Syrnck signature test")), "noncanonical signature point accepted");
}
void negative_zero() {
  // A holder can construct a valid cofactored equation with noncanonical -0
  // encoding for R. The engine's strict wire encoding must still reject it.
  const auto seed = unhex(vectors[0].seed), public_key = unhex(vectors[0].key);
  std::array<std::uint8_t, 64> expanded{}, hash{};
  std::array<std::uint8_t, 32> scalar{}, challenge{}, zero{};
  Ed25519Signature signature{}; signature[0] = 1; signature[31] = 128;
  crypto_sha512(expanded.data(), seed.data(), seed.size());
  crypto_eddsa_trim_scalar(scalar.data(), expanded.data());
  crypto_sha512_ctx state;
  crypto_sha512_init(&state); crypto_sha512_update(&state, signature.data(), 32);
  crypto_sha512_update(&state, public_key.data(), public_key.size()); crypto_sha512_final(&state, hash.data());
  crypto_eddsa_reduce(challenge.data(), hash.data());
  crypto_eddsa_mul_add(signature.data() + 32, challenge.data(), scalar.data(), zero.data());
  require(crypto_ed25519_check(signature.data(), public_key.data(), nullptr, 0) == 0,
      "noncanonical fixture did not establish the upstream verification equation");
  require(!verify_ed25519(public_key, signature, {}), "negative-zero R encoding accepted");
  crypto_wipe(expanded.data(), expanded.size()); crypto_wipe(scalar.data(), scalar.size());
}
}
int main() {
  try {
    reference_vectors(); malformed(); negative_zero();
    std::cout << "RFC Ed25519 vectors, tampering, weak keys, canonical encoding and move-only signing verified\n";
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
