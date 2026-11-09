#include "native_history_fixture.hpp"
#include <syrnck/signing_keys.hpp>
#include <iostream>
using namespace native_test;
int main() {
  try {
    const Bytes password(32, 1), next(32, 2), wrong(32, 3), seed(32, 4);
    const auto expected = SigningKey::from_seed(seed).public_key();
    // Independent Go x/crypto Argon2id + XChaCha20-Poly1305 vector:
    // password=01*32, salt=02*16, nonce=03*24, seed=04*32; public fixture only.
    const auto vector = unhex("5359525349470100000001000300000002020202020202020202020202020202"
        "030303030303030303030303030303030303030303030303"
        "fe0cae4abce4050f9fbd1b7c5b815707ed67d916d5336d9182a4e9481ec03b14"
        "bab79ae962b9eafbdfe423ce3b842b52");
    require(open_protected_signing_key(vector, password).public_key() == expected,
            "protected signer differs from independent Go envelope");
    const auto first = protect_signing_seed(seed, password);
    const auto again = protect_signing_seed(seed, password);
    require(first.public_key == expected && again.public_key == expected && first.envelope.size() == 104 &&
                first.envelope != again.envelope,
            "signing key envelope changed signer or reused randomness");
    const auto signer = open_protected_signing_key(first.envelope, password);
    const auto signature = signer.sign(bytes("authenticated signature"));
    require(signer.public_key() == expected &&
                verify_ed25519(expected, signature, bytes("authenticated signature")),
            "protected signer failed Ed25519 interoperability");
    fails([&] { open_protected_signing_key(first.envelope, wrong); });
    const auto storage = protect_secret_key(SecretKey::from_bytes(seed), password);
    fails([&] { protected_key_options(first.envelope); });
    fails([&] { protected_signing_key_options(storage); });
    fails([&] { open_secret_key(first.envelope, password); });
    fails([&] { open_protected_signing_key(storage, password); });
    auto swapped = first.envelope;
    std::copy(storage.begin(), storage.begin() + 8, swapped.begin());
    fails([&] { open_secret_key(swapped, password); }); // Purpose header is authenticated, not just parsed.
    swapped = storage;
    std::copy(first.envelope.begin(), first.envelope.begin() + 8, swapped.begin());
    fails([&] { open_protected_signing_key(swapped, password); });
    for (auto at : {6U, 9U, 12U, 16U, 32U, 56U, 88U, 103U}) {
      auto corrupt = first.envelope;
      if (at == 9)
        corrupt[at] = 4;
      else if (at == 12)
        corrupt[at] = 4;
      else
        corrupt[at] ^= 1;
      fails([&] { open_protected_signing_key(corrupt, password); });
    }
    fails([&] { open_protected_signing_key(std::span(first.envelope).first(103), password); });
    auto extended = first.envelope;
    extended.push_back(0);
    fails([&] { protected_signing_key_options(extended); });
    fails([&] { protect_signing_seed({}, password); });
    fails([&] { protect_signing_seed(seed, {}); });
    const auto generated = generate_protected_signing_key(password);
    require(generated.public_key == open_protected_signing_key(generated.envelope, password).public_key() &&
                generated.public_key != expected,
            "generated protected signer did not own a fresh identity");
    const auto rewrapped = change_signing_key_password(first.envelope, password, next);
    require(rewrapped.public_key == expected && rewrapped.envelope != first.envelope &&
                open_protected_signing_key(rewrapped.envelope, next).public_key() == expected,
            "signing password rewrap rotated the signer");
    fails([&] { open_protected_signing_key(rewrapped.envelope, password); });
    (void)open_protected_signing_key(first.envelope, password); // Old envelope was not revoked.
    const auto adjusted = protect_signing_seed(seed, password, {65 * 1024, 4});
    const auto retained = change_signing_key_password(adjusted.envelope, password, next);
    const auto costs = protected_signing_key_options(retained.envelope);
    require(costs.memory_kib == 65 * 1024 && costs.passes == 4,
            "signing rewrap changed authenticated KDF costs");
    const auto changed = change_signing_key_password(retained.envelope, next, password, PasswordKdfOptions{});
    require(protected_signing_key_options(changed.envelope).memory_kib == 64 * 1024 &&
                changed.public_key == expected,
            "explicit signing rewrap costs changed public identity");
    fails([&] { change_signing_key_password(first.envelope, wrong, next); });
    fails([&] { change_signing_key_password(first.envelope, password, {}); });
    std::cout
        << "Purpose-separated password-protected Ed25519 seeds, strict authentication and rewrap verified\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
