#pragma once
#include <syrnck/keys.hpp>
namespace syrnck::detail {
// Distinct authenticated format headers prevent storage-key/signing-seed use
// from being silently interchanged, while retaining the existing v1 costs.
enum class SecretPurpose { storage_key, signing_seed };
Bytes protect_secret_for(const SecretKey &, std::span<const std::uint8_t>, PasswordKdfOptions, SecretPurpose);
PasswordKdfOptions protected_options_for(std::span<const std::uint8_t>, SecretPurpose);
SecretKey open_secret_for(std::span<const std::uint8_t>, std::span<const std::uint8_t>, SecretPurpose);
} // namespace syrnck::detail
