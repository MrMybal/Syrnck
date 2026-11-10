#pragma once
#include "internal.hpp"
#include <syrnck/keys.hpp>
namespace syrnck::cli {
struct KeyCommand {
  std::string action, input, output;
  std::string password_env = "SYRNCK_KEY_PASSWORD", new_password_env = "SYRNCK_NEW_KEY_PASSWORD";
  std::optional<std::uint32_t> memory_mib, passes;
  bool signing = false, sealed = false;
};
Json run_key_command(const KeyCommand &);
struct VaultCommand {
  std::string action, purpose, input, output;
  std::string password_env = "SYRNCK_KEY_PASSWORD";
  std::optional<std::string> keychain;
  std::optional<std::uint32_t> memory_mib, passes;
};
Json run_vault_command(const VaultCommand &);
std::optional<Key> encryption_key_environment();
std::optional<SigningKey> protected_signing_key_environment();
std::optional<SecretKey> sealed_key_environment();
void discard_sealed_key_environment();
} // namespace syrnck::cli
