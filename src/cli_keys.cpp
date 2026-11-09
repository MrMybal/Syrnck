#include "cli_keys.hpp"
#include "native_workspace.hpp"
#include <syrnck/keys.hpp>
#include <algorithm>
#include <cstdlib>
#include <mutex>
#ifdef _WIN32
#include <Windows.h>
#endif
namespace syrnck::cli {
namespace {
struct WipeBytes { Bytes &value; ~WipeBytes() { wipe_secret(value); } };
void environment_name(std::string_view name) {
  require(!name.empty() && name.size() <= 128 &&
      ((name.front() >= 'A' && name.front() <= 'Z') || name.front() == '_') &&
      std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
      }), "key environment name must use uppercase ASCII letters, digits and underscores");
}
std::optional<Bytes> environment_value(const std::string &name, std::size_t limit) {
  environment_name(name);
#ifdef _WIN32
  const std::wstring wide_name(name.begin(), name.end());
  SetLastError(ERROR_SUCCESS);
  const auto required = GetEnvironmentVariableW(wide_name.c_str(), nullptr, 0);
  if (!required) {
    if (GetLastError() == ERROR_ENVVAR_NOT_FOUND) return {};
    require(GetLastError() == ERROR_SUCCESS, "cannot read key environment");
    return Bytes{};
  }
  require(required <= limit + 1, "key environment value exceeds its limit");
  std::vector<wchar_t> wide(required);
  struct WipeWide { std::vector<wchar_t> &value; ~WipeWide() {
    wipe_secret({reinterpret_cast<std::uint8_t *>(value.data()), value.size() * sizeof(wchar_t)});
  } } wipe_wide{wide};
  SetLastError(ERROR_SUCCESS);
  const auto length = GetEnvironmentVariableW(wide_name.c_str(), wide.data(), required);
  require(length < required && (length || GetLastError() == ERROR_SUCCESS), "cannot read key environment");
  if (!length) return Bytes{};
  const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(length), nullptr, 0, nullptr, nullptr);
  require(size > 0 && static_cast<std::size_t>(size) <= limit, "key environment value exceeds its limit");
  Bytes result(static_cast<std::size_t>(size));
  WipeBytes wipe{result};
  require(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(length),
      reinterpret_cast<char *>(result.data()), size, nullptr, nullptr) == size, "cannot read key environment");
  return result;
#else
  const auto value = std::getenv(name.c_str());
  if (!value) return {};
  const auto length = std::char_traits<char>::length(value);
  require(length <= limit, "key environment value exceeds its limit");
  return Bytes(value, value + length);
#endif
}
struct Environment {
  std::optional<Bytes> value;
  Environment(const std::string &name, std::size_t limit) : value(environment_value(name, limit)) {}
  Environment(const Environment &) = delete;
  Environment &operator=(const Environment &) = delete;
  ~Environment() { if (value) wipe_secret(*value); }
};
std::optional<SecretKey> raw_key() {
  Environment raw("SYRNCK_KEY", 64);
  if (!raw.value) return {};
  require(raw.value->size() == 64, "SYRNCK_KEY must encode exactly 32 bytes");
  auto decoded = unhex({reinterpret_cast<const char *>(raw.value->data()), raw.value->size()});
  WipeBytes wipe{decoded};
  return SecretKey::from_bytes(decoded);
}
Bytes key_file(const fs::path &path) {
  const auto status = fs::symlink_status(path);
  require(fs::is_regular_file(status) && !native_link_like(path, status), "protected key file must be a regular non-linked file");
  auto value = read_bytes(path, 104);
  (void)protected_key_options(value);
  return value;
}
fs::path output_path(const std::string &name) {
  const auto supplied = fs::absolute(path_from_utf8(name)).lexically_normal();
  require(!name.empty() && !supplied.filename().empty(), "invalid protected key output path");
  const auto parent = fs::canonical(supplied.parent_path());
  require(fs::is_directory(parent), "protected key output parent must exist");
  const auto result = parent / supplied.filename();
  require(fs::symlink_status(result).type() == fs::file_type::not_found, "protected key output already exists");
  return result;
}
Json metadata(PasswordKdfOptions options) {
  return Json{{"version",1},{"type","protected-key"},{"bytes",104},{"kdf","argon2id"},
      {"memory_kib",options.memory_kib},{"passes",options.passes},{"lanes",1},
      {"cipher","xchacha20-poly1305"},{"authenticated",false}};
}
}
std::optional<Key> encryption_key_environment() {
  Environment file("SYRNCK_KEY_FILE", 32768);
  if (!file.value) {
    auto secret = raw_key();
    if (!secret) return {};
    Key result;
    std::copy(secret->view().begin(), secret->view().end(), result.begin());
    return result;
  }
  Environment raw("SYRNCK_KEY", 64);
  require(!raw.value, "choose either SYRNCK_KEY or SYRNCK_KEY_FILE");
  require(!file.value->empty(), "SYRNCK_KEY_FILE must name a protected key file");
  Environment password("SYRNCK_KEY_PASSWORD", 4096);
  require(password.value && !password.value->empty(), "set SYRNCK_KEY_PASSWORD for the protected key file");
  const auto envelope = key_file(path_from_utf8(text(*file.value)));
  // Resident agents reopen short Store sessions. Reuse only a fully
  // authenticated key for the exact envelope AND password; never trust stamps.
  // A failed new authentication preserves the previous entry. No password or
  // derived fingerprint is logged or persisted.
  auto identity = envelope;
  WipeBytes wipe_identity{identity};
  identity.insert(identity.end(), password.value->begin(), password.value->end());
  const auto fingerprint = hash(identity);
  struct CachedKey { std::string fingerprint; SecretKey secret; };
  struct Cache { std::mutex mutex; std::optional<CachedKey> entry; };
  static Cache cache;
  const std::lock_guard lock(cache.mutex);
  if (!cache.entry || cache.entry->fingerprint != fingerprint) {
    CachedKey next{fingerprint, open_secret_key(envelope, *password.value)};
    cache.entry.emplace(std::move(next));
  }
  Key result;
  std::copy(cache.entry->secret.view().begin(), cache.entry->secret.view().end(), result.begin());
  return result;
}
Json run_key_command(const KeyCommand &request) {
  require(request.action == "create" || request.action == "protect" || request.action == "rewrap" ||
      request.action == "inspect" || request.action == "verify",
      "unknown key command; use create, protect, rewrap, inspect or verify");
  if (request.action == "inspect" || request.action == "verify") {
    const auto envelope = key_file(path_from_utf8(request.input));
    auto result = metadata(protected_key_options(envelope));
    if (request.action == "verify") {
      Environment password(request.password_env, 4096);
      require(password.value && !password.value->empty(), "set the selected key password environment variable");
      (void)open_secret_key(envelope, *password.value);
      result["authenticated"] = true;
    }
    return result;
  }
  const auto output = output_path(request.output);
  Bytes input;
  auto options = PasswordKdfOptions{};
  if (request.action == "rewrap") {
    input = key_file(path_from_utf8(request.input));
    options = protected_key_options(input);
  }
  if (request.memory_mib) {
    require(*request.memory_mib >= 64 && *request.memory_mib <= 256, "password KDF memory must be 64..256 whole MiB");
    options.memory_kib = *request.memory_mib * 1024;
  }
  if (request.passes) options.passes = *request.passes;
  options.validate();
  Environment password(request.password_env, 4096);
  require(password.value && !password.value->empty(), "set the selected key password environment variable");
  Bytes envelope;
  if (request.action == "rewrap") {
    Environment next(request.new_password_env, 4096);
    require(next.value && !next.value->empty(), "set the selected new key password environment variable");
    envelope = change_secret_key_password(input, *password.value, *next.value, options);
  } else {
    auto secret = request.action == "create" ? std::optional{SecretKey::generate()} : raw_key();
    require(secret.has_value(), "set SYRNCK_KEY before protecting an existing key");
    envelope = protect_secret_key(*secret, *password.value, options);
  }
  atomic_write(output, envelope, false);
  auto result = metadata(options);
  result["authenticated"] = true;
  result["path"] = utf8(output);
  return result;
}
} // namespace syrnck::cli
