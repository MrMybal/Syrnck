#include "sealed_internal.hpp"
#include <blake3.h>
#include <monocypher.h>
#include <algorithm>

namespace syrnck {
namespace {
Key digest(std::span<const std::uint8_t, 32> key, std::string_view domain, std::string_view value = {}) {
  blake3_hasher state;
  blake3_hasher_init_keyed(&state, key.data());
  blake3_hasher_update(&state, domain.data(), domain.size());
  const std::uint8_t zero = 0;
  blake3_hasher_update(&state, &zero, 1);
  blake3_hasher_update(&state, value.data(), value.size());
  Key out;
  blake3_hasher_finalize(&state, out.data(), out.size());
  crypto_wipe(&state, sizeof state);
  return out;
}
SecretKey derive(const SecretKey &master, std::string_view domain) {
  auto raw = digest(master.view(), domain);
  auto result = SecretKey::from_bytes(raw);
  wipe_secret(raw);
  return result;
}
Bytes associated(std::span<const std::uint8_t> header, std::string_view domain, std::string_view name) {
  Bytes result(header.begin(), header.end());
  for (const auto value : {domain, name}) {
    result.push_back(0);
    result.insert(result.end(), value.begin(), value.end());
  }
  return result;
}
} // namespace
std::string sealed_realm_id(const SecretKey &master) {
  const auto id = digest(master.view(), "Syrnck sealed realm v1");
  return hex(std::span(id).first(16));
}
namespace sealed_detail {
void Budget::work(std::size_t amount) {
  throw_if_cancelled(options.stop);
  require(amount <= limits.max_work - used, "sealed work limit exceeded");
  used += amount;
}
Cipher::Cipher(const SecretKey &master)
    : realm(sealed_realm_id(master)), addresses_(derive(master, "Syrnck sealed address key v1")),
      objects_(derive(master, "Syrnck sealed object key v1")),
      roots_(derive(master, "Syrnck sealed root key v1")) {}
std::string Cipher::address(const ObjectKey &key) const {
  validate_id(key.id);
  return hex(digest(addresses_.view(), "Syrnck sealed object address v1",
                    std::string(kind_name(key.kind)) + ':' + key.id));
}
Bytes Cipher::seal(std::string_view domain, std::string_view name,
                   std::span<const std::uint8_t> plain) const {
  require(!plain.empty() && plain.size() <= max_object_size + 33, "invalid sealed plaintext size");
  Bytes result(plain.size() + frame_overhead);
  const std::string_view magic = "SYRSEA01";
  std::copy(magic.begin(), magic.end(), result.begin());
  const auto realm_bytes = unhex(realm), nonce = random_bytes(24);
  std::copy(realm_bytes.begin(), realm_bytes.end(), result.begin() + 8);
  std::copy(nonce.begin(), nonce.end(), result.begin() + 24);
  const auto ad = associated(std::span(result).first(48), domain, name);
  const auto &key = domain == "object" ? objects_ : roots_;
  crypto_aead_lock(result.data() + 48, result.data() + 48 + plain.size(), key.view().data(), nonce.data(),
                   ad.data(), ad.size(), plain.data(), plain.size());
  return result;
}
Bytes Cipher::open(std::string_view domain, std::string_view name, std::span<const std::uint8_t> frame,
                   std::size_t maximum) const {
  require(frame.size() > frame_overhead && frame.size() - frame_overhead <= maximum,
          "invalid sealed envelope size");
  require(std::equal(frame.begin(), frame.begin() + 8, "SYRSEA01"), "invalid sealed envelope format");
  require(hex(frame.subspan(8, 16)) == realm, "sealed realm does not match the key");
  const auto ad = associated(frame.first(48), domain, name);
  const auto &key = domain == "object" ? objects_ : roots_;
  Bytes result(frame.size() - frame_overhead);
  require(crypto_aead_unlock(result.data(), frame.data() + 48 + result.size(), key.view().data(),
                             frame.data() + 24, ad.data(), ad.size(), frame.data() + 48, result.size()) == 0,
          "sealed authentication failed");
  return result;
}
} // namespace sealed_detail
} // namespace syrnck
