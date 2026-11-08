#include "cli_identity.hpp"
#include "native_workspace.hpp"
#include <monocypher.h>
#include <charconv>
#include <cstdlib>
#ifdef _WIN32
#include <Windows.h>
#endif
namespace syrnck::cli {
namespace {
std::optional<std::string> environment(const char *name) {
#ifdef _WIN32
  const std::wstring wide_name(name, name + std::char_traits<char>::length(name));
  SetLastError(ERROR_SUCCESS);
  const auto required = GetEnvironmentVariableW(wide_name.c_str(), nullptr, 0);
  if (!required) return GetLastError() == ERROR_ENVVAR_NOT_FOUND ? std::optional<std::string>{} : std::optional{std::string{}};
  require(required <= 8192, "native identity environment value exceeds its limit");
  std::wstring value(required, L'\0');
  struct Wipe { std::wstring &value; ~Wipe() { crypto_wipe(value.data(), value.size() * sizeof(wchar_t)); } } wipe{value};
  const auto length = GetEnvironmentVariableW(wide_name.c_str(), value.data(), required);
  require(length && length < required, "cannot read native identity environment");
  const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(length), nullptr, 0, nullptr, nullptr);
  require(size > 0, "cannot read native identity environment");
  std::string result(static_cast<std::size_t>(size), '\0');
  require(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(length), result.data(), size, nullptr, nullptr) == size,
      "cannot read native identity environment");
  return result;
#else
  const auto value = std::getenv(name);
  if (!value) return {};
  require(std::char_traits<char>::length(value) <= 8192, "native identity environment value exceeds its limit");
  return std::string(value);
#endif
}
fs::path file(const Store &store) { return store.root() / "native-identity.json"; }
bool profile_present(const fs::path &path) {
  const auto status = fs::symlink_status(path);
  if (status.type() == fs::file_type::not_found) return false;
  require(fs::is_regular_file(status) && !native_link_like(path, status), "invalid native identity profile file");
  return true;
}
Json profile(const NativeIdentity &identity) {
  identity.validate();
  return Json{{"version", 1}, {"type", "native-identity"}, {"name", identity.name},
      {"email", identity.email}, {"timezone_offset", identity.timezone_offset}};
}
std::optional<NativeIdentity> read_profile(const Store &store) {
  const auto path = file(store);
  if (!profile_present(path)) return {};
  const auto data = read_bytes(path, 4096);
  const auto value = parse_json(data);
  require(value.is_object() && value.size() == 5 && value.value("version", 0) == 1 &&
      value.value("type", "") == "native-identity", "invalid native identity profile");
  const auto timezone = value.at("timezone_offset").dump();
  NativeIdentity identity{value.at("name").get<std::string>(), value.at("email").get<std::string>(), native_timezone_offset(timezone)};
  require(profile(identity).dump() == text(data), "invalid native identity profile");
  return identity;
}
}
std::int32_t native_timezone_offset(std::string_view value) {
  if (value.starts_with('+')) value.remove_prefix(1);
  require(!value.empty(), "native identity timezone offset must be in -1439..1439");
  std::int32_t result{};
  const auto converted = std::from_chars(value.data(), value.data() + value.size(), result);
  require(converted.ec == std::errc{} && converted.ptr == value.data() + value.size() && result >= -1439 && result <= 1439,
      "native identity timezone offset must be in -1439..1439");
  return result;
}
std::optional<SigningKey> signing_key_environment() {
  auto value = environment("SYRNCK_SIGNING_SEED");
  if (!value) return {};
  struct Wipe { std::string &value; ~Wipe() { crypto_wipe(value.data(), value.size()); } } wipe{*value};
  discard_signing_seed_environment();
  require(value->size() == 64, "SYRNCK_SIGNING_SEED must encode exactly 32 bytes");
  auto seed = unhex(*value);
  struct WipeSeed { Bytes &value; ~WipeSeed() { crypto_wipe(value.data(), value.size()); } } wipe_seed{seed};
  return SigningKey::from_seed(seed);
}
void discard_signing_seed_environment() {
#ifdef _WIN32
  SetLastError(ERROR_SUCCESS);
  const auto result = SetEnvironmentVariableW(L"SYRNCK_SIGNING_SEED", nullptr);
  require((result != 0 || GetLastError() == ERROR_ENVVAR_NOT_FOUND) && _putenv_s("SYRNCK_SIGNING_SEED", "") == 0,
      "cannot clear signing seed environment");
#else
  require(::unsetenv("SYRNCK_SIGNING_SEED") == 0, "cannot clear signing seed environment");
#endif
}
void load_native_identity(Store &store) {
  auto identity = read_profile(store);
  const auto name = environment("SYRNCK_USER_NAME"), email = environment("SYRNCK_USER_EMAIL");
  require(name.has_value() == email.has_value(), "set both SYRNCK_USER_NAME and SYRNCK_USER_EMAIL");
  if (name) identity = NativeIdentity{*name, *email, identity ? identity->timezone_offset : 0};
  if (const auto timezone = environment("SYRNCK_USER_TIMEZONE")) {
    require(identity.has_value(), "native timezone requires an identity");
    identity->timezone_offset = native_timezone_offset(*timezone);
  }
  auto signer = signing_key_environment();
  require(identity || !signer, "signing seed requires a native identity");
  if (identity) store.use_native_identity(std::move(*identity), std::move(signer));
}
void save_native_identity(const Store &store, const NativeIdentity &identity, OperationOptions options) {
  throw_if_cancelled(options.stop); require_no_native_checkout(store);
  const auto value = profile(identity);
  const auto path = file(store);
  (void)profile_present(path);
  store.require_features({"authored-revisions-v1"}, options);
  report_progress(options, {ProgressPhase::publishing, "native-identity"});
  (void)profile_present(path);
  atomic_write(path, bytes(value.dump()), true, options.stop);
}
void remove_native_identity(const Store &store, OperationOptions options) {
  throw_if_cancelled(options.stop); require_no_native_checkout(store);
  const auto path = file(store);
  if (!profile_present(path)) return;
  report_progress(options, {ProgressPhase::publishing, "native-identity"});
  if (!profile_present(path)) return;
  require(fs::remove(path), "cannot remove native identity profile"); sync_directory(path.parent_path());
}
} // namespace syrnck::cli
