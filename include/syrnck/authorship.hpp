#pragma once
#include <syrnck/signing.hpp>
#include <cstdint>
#include <optional>
#include <string>
namespace syrnck {
struct NativeIdentity {
  std::string name, email;
  std::int32_t timezone_offset = 0; // minutes east of UTC
  void validate() const;
};
// Move-only process state. A resident agent can carry its signing key between
// short-lived Store handles without exporting or persisting secret bytes.
struct NativeIdentityContext {
  NativeIdentity identity;
  std::optional<SigningKey> signer;
  bool require_signature = false;
};
} // namespace syrnck
