#pragma once
#include "internal.hpp"
namespace syrnck::cli {
struct KeyCommand {
  std::string action, input, output;
  std::string password_env = "SYRNCK_KEY_PASSWORD", new_password_env = "SYRNCK_NEW_KEY_PASSWORD";
  std::optional<std::uint32_t> memory_mib, passes;
};
Json run_key_command(const KeyCommand &);
std::optional<Key> encryption_key_environment();
} // namespace syrnck::cli
