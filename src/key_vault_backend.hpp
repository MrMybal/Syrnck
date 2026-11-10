#pragma once
#include <syrnck/key_vault.hpp>
namespace syrnck::detail {
inline constexpr std::size_t vault_record_size = 73;
SecretKey decode_vault_record(const VaultKeyReference &, std::span<const std::uint8_t>);
// Only exact, validated Syrnck references reach these platform operations.
// Returned buffers contain secrets and must be wiped by the caller.
std::optional<Bytes> vault_read(const std::optional<fs::path> &, const VaultKeyReference &);
void vault_write(const std::optional<fs::path> &, const VaultKeyReference &, std::span<const std::uint8_t>);
bool vault_erase(const std::optional<fs::path> &, const VaultKeyReference &);
std::string_view vault_provider() noexcept;
} // namespace syrnck::detail
