#include "native_history_fixture.hpp"
#include <syrnck/keys.hpp>
#include <iostream>
#include <type_traits>
#include <utility>
using namespace native_test;
static_assert(!std::is_copy_constructible_v<SecretKey> && !std::is_copy_assignable_v<SecretKey>);
static_assert(std::is_nothrow_move_constructible_v<SecretKey> && std::is_nothrow_move_assignable_v<SecretKey>);
template<class T> concept BorrowableSecret = requires(T &&owner) { std::forward<T>(owner).view(); };
static_assert(BorrowableSecret<SecretKey &> && BorrowableSecret<const SecretKey &>);
static_assert(!BorrowableSecret<SecretKey> && !BorrowableSecret<const SecretKey>);
int main() {
  Temporary temporary;
  try {
    // Public synthetic interoperability vector, generated independently with
    // golang.org/x/crypto Argon2.IDKey and chacha20poly1305.NewX (v0.57.0).
    // Password=01*32, salt=02*16, nonce=03*24, plaintext=04*32. Not a deployment key.
    Bytes password(32, 1), salt(16, 2), raw(32, 4), other_password(32, 5);
    const auto derived = SecretKey::from_password(password, salt);
    require(hex(derived.view()) == "577a81cb3dc2546446cda5871efa74c3393a40b1708589be07bd4c95afd1223e",
        "Argon2id differs from independent Go vector");
    const auto envelope = unhex("5359524b45590100000001000300000002020202020202020202020202020202"
        "030303030303030303030303030303030303030303030303"
        "fe0cae4abce4050f9fbd1b7c5b815707ed67d916d5336d9182a4e9481ec03b14"
        "0cc00c24eaa4f5857db1abe23748fdf7");
    auto opened = open_secret_key(envelope, password);
    require(std::equal(raw.begin(), raw.end(), opened.view().begin()), "Go XChaCha20-Poly1305 envelope differs");
    auto owned = SecretKey::from_bytes(raw);
    wipe_secret(raw);
    require(std::all_of(raw.begin(), raw.end(), [](auto byte) { return byte == 0; }) &&
        std::all_of(owned.view().begin(), owned.view().end(), [](auto byte) { return byte == 4; }),
        "secret input retained or wipe failed");
    auto moved = std::move(owned);
    fails([&] { (void)owned.view(); });
    auto destination = SecretKey::generate();
    destination = std::move(moved);
    fails([&] { (void)moved.view(); });
    require(std::equal(destination.view().begin(), destination.view().end(), opened.view().begin()),
        "secret move changed bytes");
    const auto protected_once = protect_secret_key(destination, password);
    const auto protected_twice = protect_secret_key(destination, password);
    require(protected_once.size() == 104 && protected_twice.size() == 104 && protected_once != protected_twice &&
        !std::equal(protected_once.begin() + 16, protected_once.begin() + 56, protected_twice.begin() + 16),
        "key protection reused salt/nonce");
    const auto recovered = open_secret_key(protected_once, password);
    require(std::equal(recovered.view().begin(), recovered.view().end(), destination.view().begin()),
        "protected key round trip changed bytes");
    fails([&] { open_secret_key(protected_once, other_password); });
    for (const auto at : {0U, 6U, 9U, 12U, 16U, 32U, 56U, 88U, 103U}) {
      auto changed = envelope;
      if (at == 12) changed[at] = 4; // Valid alternate cost, still authenticated.
      else if (at == 9) changed[at] = 4; // Valid 65 MiB memory cost.
      else changed[at] ^= 1;
      fails([&] { open_secret_key(changed, password); });
    }
    fails([&] { open_secret_key(std::span(envelope).first(103), password); });
    auto extended = envelope; extended.push_back(0);
    fails([&] { open_secret_key(extended, password); });
    for (auto memory : {0U, 65535U, 65537U, 262145U, 0xffffffffU})
      fails([&] { SecretKey::from_password(password, salt, {memory, 3}); });
    for (auto passes : {0U, 2U, 11U, 0xffffffffU})
      fails([&] { SecretKey::from_password(password, salt, {65536, passes}); });
    fails([&] { SecretKey::from_bytes({}); });
    fails([&] { SecretKey::from_password({}, salt); });
    fails([&] { SecretKey::from_password(Bytes(4097, 1), salt); });
    fails([&] { SecretKey::from_password(password, Bytes(15, 2)); });
    fails([&] { SecretKey::from_password(password, Bytes(17, 2)); });
    const auto rewrapped = change_secret_key_password(envelope, password, other_password);
    const auto rewrapped_key = open_secret_key(rewrapped, other_password);
    require(std::equal(rewrapped_key.view().begin(), rewrapped_key.view().end(), destination.view().begin()),
        "password change rotated the underlying storage key");
    fails([&] { open_secret_key(rewrapped, password); });
    (void)open_secret_key(envelope, password); // An old envelope is not revoked.
    const auto adjusted = protect_secret_key(destination, password, {65 * 1024, 4});
    const auto retained_costs = change_secret_key_password(adjusted, password, other_password);
    require(std::equal(adjusted.begin(), adjusted.begin() + 16, retained_costs.begin()),
        "password change silently changed KDF costs");
    (void)open_secret_key(retained_costs, other_password);
    Key storage_key{};
    std::copy(destination.view().begin(), destination.view().end(), storage_key.begin());
    ObjectKey manifest;
    {
      Store store(temporary.path / "store", storage_key, true);
      std::istringstream input("verified password-protected storage bytes");
      manifest = {Kind::manifest, store.ingest(input).first};
      store.set_ref("retained", manifest);
    }
    wipe_secret(storage_key);
    std::copy(rewrapped_key.view().begin(), rewrapped_key.view().end(), storage_key.begin());
    {
      Store reopened(temporary.path / "store", storage_key);
      std::ostringstream output;
      reopened.reconstruct(manifest.id, output);
      require(output.str() == "verified password-protected storage bytes" && reopened.refs().at("retained") == manifest,
          "password rewrap lost encrypted Store bytes or roots");
    }
    wipe_secret(storage_key);
    wipe_secret({});
    std::cout << "Independent Argon2id/AEAD vectors, move ownership, strict headers and retained encrypted Store verified\n";
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
