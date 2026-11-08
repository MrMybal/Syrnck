#pragma once
#include "internal.hpp"
namespace syrnck::cli {
std::int32_t native_timezone_offset(std::string_view);
std::optional<SigningKey> signing_key_environment();
void discard_signing_seed_environment();
void load_native_identity(Store &);
void save_native_identity(const Store &, const NativeIdentity &, OperationOptions = {});
void remove_native_identity(const Store &, OperationOptions = {});
} // namespace syrnck::cli
