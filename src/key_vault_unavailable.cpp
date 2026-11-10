#include "key_vault_backend.hpp"
namespace syrnck::detail {
std::string_view vault_provider() noexcept { return "unavailable"; }
std::optional<Bytes> vault_read(const std::optional<fs::path> &, const VaultKeyReference &) {
  throw Error("system key vault support is not available in this build");
}
void vault_write(const std::optional<fs::path> &, const VaultKeyReference &, std::span<const std::uint8_t>) {
  throw Error("system key vault support is not available in this build");
}
bool vault_erase(const std::optional<fs::path> &, const VaultKeyReference &) {
  throw Error("system key vault support is not available in this build");
}
} // namespace syrnck::detail
