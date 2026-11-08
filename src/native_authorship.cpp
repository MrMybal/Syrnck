#include "native_authorship.hpp"
#include <utf8proc.h>
namespace syrnck {
namespace {
void text(std::string_view value, std::size_t maximum) {
  require(!value.empty() && value.size() <= maximum, "invalid native identity text length");
  for (std::size_t position = 0; position < value.size();) {
    utf8proc_int32_t codepoint;
    const auto size = utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t *>(value.data() + position),
        static_cast<utf8proc_ssize_t>(value.size() - position), &codepoint);
    require(size > 0 && codepoint >= 32 && (codepoint < 127 || codepoint > 159), "invalid native identity text");
    position += static_cast<std::size_t>(size);
  }
}
std::uint64_t timestamp(const Json &value) {
  require(value.is_number_unsigned() || (value.is_number_integer() && value.get<std::int64_t>() >= 0),
      "invalid native author timestamp");
  return value.get<std::uint64_t>();
}
std::int32_t offset(const Json &value) {
  require(value.is_number_integer(), "invalid native author timezone offset");
  if (value.is_number_unsigned()) {
    const auto number = value.get<std::uint64_t>();
    require(number <= 1439, "invalid native author timezone offset");
    return static_cast<std::int32_t>(number);
  }
  const auto number = value.get<std::int64_t>();
  require(number >= -1439 && number <= 1439, "invalid native author timezone offset");
  return static_cast<std::int32_t>(number);
}
}
void NativeIdentity::validate() const {
  text(name, 256); text(email, 320);
  require(timezone_offset >= -1439 && timezone_offset <= 1439, "invalid native author timezone offset");
}
Json native_author_stamp(const NativeIdentity &identity, const Json &time) {
  identity.validate();
  return Json{{"name", identity.name}, {"email", identity.email}, {"timestamp", timestamp(time)},
      {"timezone_offset", identity.timezone_offset}};
}
Json canonical_native_author(const Json &value, bool nullable) {
  if (nullable && value.is_null()) return {};
  require(value.is_object() && value.size() == 4 && value.contains("name") && value.contains("email") &&
      value.contains("timestamp") && value.contains("timezone_offset"), "invalid native author structure");
  const NativeIdentity identity{value.at("name").get<std::string>(), value.at("email").get<std::string>(), offset(value.at("timezone_offset"))};
  return native_author_stamp(identity, value.at("timestamp"));
}
std::string native_revision_signing_message(const Json &unsigned_revision) {
  return "Syrnck native revision signature v1:" + hash(bytes(unsigned_revision.dump()));
}
Json canonical_native_signature(const Json &value, const Json &unsigned_revision) {
  if (value.is_null()) return {};
  require(value.is_object() && value.size() == 4 && value.contains("algorithm") && value.contains("key_id") &&
      value.contains("public_key") && value.contains("value"), "invalid native revision signature structure");
  require(value.at("algorithm").is_string() && value.at("algorithm") == "ed25519", "unsupported native revision signature algorithm");
  const auto public_key = value.at("public_key").get<std::string>(), encoded = value.at("value").get<std::string>();
  validate_id(public_key);
  require(encoded.size() == 128, "invalid native revision signature length");
  const auto key = unhex(public_key), signature = unhex(encoded);
  require(hex(signature) == encoded, "noncanonical native revision signature");
  const auto identifier = signing_key_id(key);
  require(value.at("key_id").is_string() && value.at("key_id") == identifier, "native revision signing key identifier mismatch");
  const auto message = native_revision_signing_message(unsigned_revision);
  require(verify_ed25519(key, signature, bytes(message)), "invalid native revision signature");
  return Json{{"algorithm", "ed25519"}, {"key_id", identifier}, {"public_key", public_key}, {"value", encoded}};
}
} // namespace syrnck
