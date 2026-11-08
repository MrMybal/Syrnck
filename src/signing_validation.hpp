#pragma once
#include <cstdint>
#include <span>
namespace syrnck {
// Configuration-time validation: canonical curve point and non-small order.
// Signature verification independently validates each actual signed message.
bool valid_ed25519_public_key(std::span<const std::uint8_t>) noexcept;
} // namespace syrnck
