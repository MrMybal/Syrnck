#include <cstring>
#include <fstream>
#include <algorithm>
#include <streambuf>
#include <syrnck.h>
#include <syrnck/core.hpp>
#include <syrnck/keys.hpp>
#include <syrnck/signing_keys.hpp>
#include "key_envelope.hpp"
#include <syrnck/watcher.hpp>
#include <syrnck/workspace.hpp>
#include "native_workspace.hpp"
struct SyrnckStore {
  syrnck::Store store;
  SyrnckStore(const syrnck::fs::path &p, const std::optional<syrnck::Key> &key, bool create)
      : store(p, key, create) {}
};
struct SyrnckSecretKey {
  syrnck::SecretKey key;
  explicit SyrnckSecretKey(syrnck::SecretKey value) : key(std::move(value)) {}
};
struct SyrnckSigningKey {
  syrnck::SecretKey seed;
  explicit SyrnckSigningKey(syrnck::SecretKey value) : seed(std::move(value)) {}
};
struct SyrnckCancellation { std::stop_source source; };
struct SyrnckWorkspace {
  syrnck::fs::path directory, storage;
  std::string context;
  syrnck::WorkspaceWatcher watcher;
  syrnck::WorkspaceObservation observation;
  syrnck::WorkspaceCapture capture;
  syrnck::WorkspaceChanges pending;
  std::size_t max_paths;
  SyrnckWorkspace(const syrnck::fs::path &root, const syrnck::fs::path &storage,
                  syrnck::WorkspaceWatchOptions options, std::stop_token stop, std::string name = {})
      : directory(syrnck::fs::canonical(root)), storage(syrnck::fs::canonical(storage)), context(std::move(name)), watcher(directory, storage, options, stop),
        observation(directory, storage), capture(directory, storage), max_paths(options.max_paths) { invalidate(); }
  void invalidate() {
    observation.invalidate();
    capture.invalidate();
    pending.full_scan = true;
  }
};
namespace {
// No heap allocation is needed to enter a call or report its failure. In
// particular, a bad_alloc must not escape while building an error string.
thread_local std::array<char, 4096> last_error{};
void set_error(const char *message) noexcept {
  auto length = std::min(std::strlen(message), last_error.size() - 1);
  std::memcpy(last_error.data(), message, length);
  last_error[length] = 0;
}
template <class Operation> int boundary(const Operation &operation) noexcept {
  try {
    operation();
    last_error[0] = 0;
    return 0;
  } catch (const syrnck::Cancelled &e) {
    set_error(e.what());
    return SYRNCK_CANCELLED;
  } catch (const std::exception &e) {
    set_error(e.what());
    return 1;
  } catch (...) {
    set_error("unknown C++ exception");
    return 2;
  }
}
const char *checked(const char *s) {
  syrnck::require(s != nullptr, "null string");
  return s;
}
std::span<const std::uint8_t> key_password(const unsigned char *value, std::size_t length) {
  syrnck::require(value != nullptr && length >= 1 && length <= 4096, "key password must contain 1..4096 bytes");
  return {value, length};
}
syrnck::PasswordKdfOptions key_costs(const SyrnckPasswordKdf *options, syrnck::PasswordKdfOptions result = {}) {
  if (options) {
    if (options->memory_kib) result.memory_kib = options->memory_kib;
    if (options->passes) result.passes = options->passes;
  }
  result.validate();
  return result;
}
std::span<const std::uint8_t> key_envelope(const unsigned char *value, std::size_t length, bool signing = false) {
  syrnck::require(value != nullptr && length == SYRNCK_PROTECTED_KEY_SIZE, "invalid protected key envelope");
  const std::span<const std::uint8_t> result{value, length};
  if (signing) (void)syrnck::protected_signing_key_options(result);
  else (void)syrnck::protected_key_options(result);
  return result;
}
bool key_output(unsigned char *out, std::size_t capacity, std::size_t *length) {
  syrnck::require(length != nullptr, "null size output");
  *length = SYRNCK_PROTECTED_KEY_SIZE;
  if (!out && capacity == 0) return false;
  syrnck::require(out && capacity >= SYRNCK_PROTECTED_KEY_SIZE, "insufficient output capacity");
  *length = 0;
  return true;
}
syrnck::Store &checked(SyrnckStore *p) {
  syrnck::require(p != nullptr, "null store handle");
  return p->store;
}
SyrnckWorkspace &checked(SyrnckWorkspace *p) {
  syrnck::require(p != nullptr, "null workspace handle");
  return *p;
}
std::stop_token stop_token(const SyrnckCancellation *cancellation) {
  return cancellation ? cancellation->source.get_token() : std::stop_token{};
}
std::uint32_t progress_phase(syrnck::ProgressPhase phase) {
  switch (phase) {
  case syrnck::ProgressPhase::scanning: return SYRNCK_PHASE_SCANNING;
  case syrnck::ProgressPhase::ingesting: return SYRNCK_PHASE_INGESTING;
  case syrnck::ProgressPhase::reconstructing: return SYRNCK_PHASE_RECONSTRUCTING;
  case syrnck::ProgressPhase::publishing: return SYRNCK_PHASE_PUBLISHING;
  case syrnck::ProgressPhase::verifying: return SYRNCK_PHASE_VERIFYING;
  case syrnck::ProgressPhase::exporting: return SYRNCK_PHASE_EXPORTING;
  case syrnck::ProgressPhase::transferring: return SYRNCK_PHASE_TRANSFERRING;
  case syrnck::ProgressPhase::planning: return SYRNCK_PHASE_PLANNING;
  case syrnck::ProgressPhase::packing: return SYRNCK_PHASE_PACKING;
  }
  throw syrnck::Error("invalid operation progress phase");
}
syrnck::OperationOptions operation_options(const SyrnckOperation *operation) {
  syrnck::OperationOptions result;
  if (!operation) return result;
  result.stop = stop_token(operation->cancellation);
  if (operation->progress) {
    const auto callback = operation->progress;
    const auto user = operation->user;
    result.progress = [callback, user](const syrnck::ProgressEvent &event) {
      const SyrnckProgress value{progress_phase(event.phase), event.path.data(), event.path.size(),
          event.bytes, event.total_bytes.value_or(0), event.total_bytes ? 1u : 0u,
          event.complete ? 1u : 0u, event.buffered_blocks};
      callback(&value, user);
    };
  }
  return result;
}
void json_output(const syrnck::Json &value, char **out, std::size_t *length, std::stop_token stop) {
  syrnck::throw_if_cancelled(stop);
  auto text = value.dump();
  syrnck::throw_if_cancelled(stop);
  syrnck::require(text.size() <= 128 * 1024 * 1024, "workspace output exceeds 128 MiB");
  auto result = std::make_unique<char[]>(text.size() + 1);
  std::memcpy(result.get(), text.c_str(), text.size() + 1);
  syrnck::throw_if_cancelled(stop);
  *length = text.size() + 1;
  *out = result.release();
}
template <class Operation> int workspace_boundary(SyrnckWorkspace *workspace, char **out,
                                                  std::size_t *length, const Operation &operation) noexcept {
  if (out) *out = nullptr;
  if (length) *length = 0;
  const auto result = boundary([&] {
    syrnck::require(out != nullptr, "null JSON output");
    syrnck::require(length != nullptr, "null size output");
    operation(checked(workspace));
  });
  if (result && workspace) workspace->invalidate();
  return result;
}
syrnck::Store &workspace_store(SyrnckWorkspace &session, SyrnckStore *handle) {
  auto &store = checked(handle);
  syrnck::require(syrnck::fs::canonical(store.root()) == session.storage,
                  "workspace observation belongs to another store");
  syrnck::require(store.native_workspace_name() == session.context, "workspace observation belongs to another context");
  store.validate_native_workspace(session.directory);
  const auto head = store.native_head();
  syrnck::require(head["directory"].is_null() || head["directory"].get<std::string>() == syrnck::utf8(session.directory),
                  "native HEAD belongs to another workspace");
  return store;
}
std::string expected_native_target(const syrnck::Store &store, const SyrnckNativeExpected *expected) {
  syrnck::require(expected != nullptr, "null native workspace expectation");
  const std::string target = checked(expected->target);
  syrnck::validate_native_target(target);
  syrnck::require(store.native_target() == target, "native workspace HEAD selection changed");
  std::optional<syrnck::ObjectKey> old;
  if (expected->revision) {
    syrnck::validate_id(expected->revision);
    old = syrnck::ObjectKey{syrnck::Kind::revision, expected->revision};
  }
  const auto refs = store.refs();
  const auto at = refs.find(syrnck::native_revision_reference(store, target));
  const auto current = at == refs.end() ? std::optional<syrnck::ObjectKey>{} : std::optional{at->second};
  syrnck::require(current == old, "native workspace revision changed; reload and replan");
  const auto materialization = store.native_materialization()["object"];
  if (expected->materialization) syrnck::validate_id(expected->materialization);
  syrnck::require(materialization == (expected->materialization ? syrnck::Json(expected->materialization) : syrnck::Json{}),
      "native workspace materialization changed; reload and replan");
  return target;
}
syrnck::ObjectKey key(const char *kind, const char *id) {
  syrnck::ObjectKey result{syrnck::parse_kind(checked(kind)), checked(id)};
  syrnck::validate_id(result.id);
  return result;
}
void copy_bytes(std::span<const std::uint8_t> bytes, void *out, std::size_t capacity, std::size_t *length) {
  *length = bytes.size();
  if (!out && capacity == 0)
    return;
  syrnck::require(out && capacity >= bytes.size(), "insufficient output capacity");
  if (!bytes.empty())
    std::memcpy(out, bytes.data(), bytes.size());
}
class BufferOutput : public std::streambuf {
public:
  explicit BufferOutput(syrnck::Bytes &data) {
    if (!data.empty()) {
      auto first = reinterpret_cast<char *>(data.data());
      setp(first, first + data.size());
    }
  }
};
class BufferInput : public std::streambuf {
public:
  explicit BufferInput(std::span<const std::uint8_t> data) {
    if (!data.empty()) {
      auto first = const_cast<char *>(reinterpret_cast<const char *>(data.data()));
      setg(first, first, first + data.size());
    }
  }
};
syrnck::Chunking chunking_options(const SyrnckChunking *options) {
  syrnck::Chunking result;
  if (options) {
    syrnck::require(options->algorithm <= 1, "unknown chunking algorithm");
    result.fixed = options->algorithm == 1;
    result.min = options->min; result.avg = options->average; result.max = options->max;
    result.validate();
    if (result.fixed)
      syrnck::require(result.min == result.avg && result.avg == result.max, "fixed chunking sizes must match");
  }
  return result;
}
class DiscardOutput : public std::streambuf {
  std::streamsize xsputn(const char *, std::streamsize length) override {
    return length;
  }
  int_type overflow(int_type value) override {
    return traits_type::not_eof(value);
  }
};
} // namespace
extern "C" {
uint32_t syrnck_abi_version(void) {
  return SYRNCK_ABI_VERSION;
}
const char *syrnck_last_error(void) {
  return last_error.data();
}
int syrnck_error_copy(char *out, std::size_t capacity, std::size_t *required) {
  if (!required)
    return 1;
  *required = std::strlen(last_error.data()) + 1;
  if (!out && capacity == 0)
    return 0;
  if (!out || capacity < *required)
    return 1;
  std::memcpy(out, last_error.data(), *required);
  return 0;
}
int syrnck_store_open(const char *path, bool create, const unsigned char *key, std::size_t length,
                      SyrnckStore **out) {
  return boundary([&] {
    syrnck::require(out != nullptr, "null output handle");
    *out = nullptr;
    std::optional<syrnck::Key> secret;
    struct Wipe { std::optional<syrnck::Key> &value; ~Wipe() { if (value) syrnck::wipe_secret(*value); } } wipe{secret};
    if (length) {
      syrnck::require(length == 32 && key != nullptr, "key_len must be 0 or 32 with a valid key");
      secret.emplace();
      std::copy_n(key, 32, secret->begin());
    }
    *out = new SyrnckStore(syrnck::path_from_utf8(checked(path)), secret, create);
  });
}
int syrnck_key_protect(const unsigned char *key, std::size_t key_length,
    const unsigned char *password, std::size_t password_length, const SyrnckPasswordKdf *options,
    unsigned char *out, std::size_t capacity, std::size_t *length) {
  if (length) *length = 0;
  return boundary([&] {
    const auto cost = key_costs(options);
    const auto pass = key_password(password, password_length);
    syrnck::require((!key && key_length == 0) || (key && key_length == 32), "secret key length must be zero or 32 with a valid key");
    if (!key_output(out, capacity, length)) return;
    const auto secret = key_length ? syrnck::SecretKey::from_bytes({key, key_length}) : syrnck::SecretKey::generate();
    const auto envelope = syrnck::protect_secret_key(secret, pass, cost);
    std::memcpy(out, envelope.data(), envelope.size());
    *length = envelope.size();
  });
}
int syrnck_key_change_password(const unsigned char *envelope, std::size_t envelope_length,
    const unsigned char *old_password, std::size_t old_length, const unsigned char *new_password,
    std::size_t new_length, const SyrnckPasswordKdf *options,
    unsigned char *out, std::size_t capacity, std::size_t *length) {
  if (length) *length = 0;
  return boundary([&] {
    const auto input = key_envelope(envelope, envelope_length);
    const auto before = key_password(old_password, old_length), after = key_password(new_password, new_length);
    std::optional<syrnck::PasswordKdfOptions> costs;
    if (options) costs = key_costs(options, syrnck::protected_key_options(input));
    if (!key_output(out, capacity, length)) return;
    const auto result = syrnck::change_secret_key_password(input, before, after, costs);
    std::memcpy(out, result.data(), result.size());
    *length = result.size();
  });
}
int syrnck_signing_key_protect(const unsigned char *seed, std::size_t seed_length,
    const unsigned char *password, std::size_t password_length, const SyrnckPasswordKdf *options,
    unsigned char *out, std::size_t capacity, std::size_t *length) {
  if (length) *length = 0;
  return boundary([&] {
    const auto cost = key_costs(options);
    const auto pass = key_password(password, password_length);
    syrnck::require((!seed && seed_length == 0) || (seed && seed_length == 32),
        "signing seed length must be 0 or 32");
    if (!key_output(out, capacity, length)) return;
    const auto result = seed_length ? syrnck::protect_signing_seed({seed, seed_length}, pass, cost)
                                   : syrnck::generate_protected_signing_key(pass, cost);
    std::memcpy(out, result.envelope.data(), result.envelope.size());
    *length = result.envelope.size();
  });
}
int syrnck_signing_key_change_password(const unsigned char *envelope, std::size_t envelope_length,
    const unsigned char *old_password, std::size_t old_length, const unsigned char *new_password,
    std::size_t new_length, const SyrnckPasswordKdf *options,
    unsigned char *out, std::size_t capacity, std::size_t *length) {
  if (length) *length = 0;
  return boundary([&] {
    const auto input = key_envelope(envelope, envelope_length, true);
    const auto before = key_password(old_password, old_length), after = key_password(new_password, new_length);
    std::optional<syrnck::PasswordKdfOptions> costs;
    if (options) costs = key_costs(options, syrnck::protected_signing_key_options(input));
    if (!key_output(out, capacity, length)) return;
    const auto result = syrnck::change_signing_key_password(input, before, after, costs);
    std::memcpy(out, result.envelope.data(), result.envelope.size());
    *length = result.envelope.size();
  });
}
int syrnck_signing_key_open(const unsigned char *envelope, std::size_t envelope_length,
    const unsigned char *password, std::size_t password_length, SyrnckSigningKey **out) {
  if (out) *out = nullptr;
  return boundary([&] {
    syrnck::require(out != nullptr, "null output handle");
    const auto input = key_envelope(envelope, envelope_length, true), pass = key_password(password, password_length);
    *out = new SyrnckSigningKey(syrnck::detail::open_secret_for(input, pass,
        syrnck::detail::SecretPurpose::signing_seed));
  });
}
int syrnck_signing_key_close(SyrnckSigningKey *key) { return boundary([&] { delete key; }); }
int syrnck_signing_key_public_json(const SyrnckSigningKey *key, char **out, std::size_t *length) {
  if (out) *out = nullptr;
  if (length) *length = 0;
  return boundary([&] {
    syrnck::require(out != nullptr && length != nullptr, "null identity output");
    syrnck::require(key != nullptr, "null signing key handle");
    const auto public_key = syrnck::SigningKey::from_seed(key->seed.view()).public_key();
    json_output(syrnck::Json{{"version", 1}, {"algorithm", "ed25519"}, {"public_key", syrnck::hex(public_key)},
        {"key_id", syrnck::signing_key_id(public_key)}}, out, length, {});
  });
}
int syrnck_secret_key_open(const unsigned char *envelope, std::size_t envelope_length,
    const unsigned char *password, std::size_t password_length, SyrnckSecretKey **out) {
  if (out) *out = nullptr;
  return boundary([&] {
    syrnck::require(out != nullptr, "null output handle");
    const auto input = key_envelope(envelope, envelope_length), pass = key_password(password, password_length);
    *out = new SyrnckSecretKey(syrnck::open_secret_key(input, pass));
  });
}
int syrnck_secret_key_close(SyrnckSecretKey *key) { return boundary([&] { delete key; }); }
int syrnck_store_open_secret(const char *path, bool create, const SyrnckSecretKey *key, SyrnckStore **out) {
  if (out) *out = nullptr;
  return boundary([&] {
    syrnck::require(out != nullptr, "null output handle");
    const auto directory = syrnck::path_from_utf8(checked(path));
    syrnck::require(key != nullptr, "null secret key handle");
    std::optional<syrnck::Key> secret{std::in_place};
    struct Wipe { syrnck::Key &value; ~Wipe() { syrnck::wipe_secret(value); } } wipe{*secret};
    std::copy(key->key.view().begin(), key->key.view().end(), secret->begin());
    *out = new SyrnckStore(directory, secret, create);
  });
}
int syrnck_store_close(SyrnckStore *store) {
  return boundary([&] { delete store; });
}
int syrnck_store_use_workspace(SyrnckStore *p, const char *name) {
  return boundary([&] { checked(p).use_native_workspace(name ? name : ""); });
}
int syrnck_store_use_native_identity(SyrnckStore *p, const char *name, const char *email,
    std::int32_t offset, const unsigned char *seed, std::size_t length, bool required) {
  return boundary([&] {
    auto &store = checked(p);
    syrnck::NativeIdentity identity{checked(name), checked(email), offset};
    identity.validate();
    syrnck::require(length == 0 || (length == 32 && seed != nullptr), "signing seed length must be 0 or 32");
    std::optional<syrnck::SigningKey> signer;
    if (length) signer.emplace(syrnck::SigningKey::from_seed({seed, length}));
    store.use_native_identity(std::move(identity), std::move(signer), required);
  });
}
int syrnck_store_use_native_signer(SyrnckStore *p, const char *name, const char *email,
    std::int32_t offset, const SyrnckSigningKey *key, bool required) {
  return boundary([&] {
    auto &store = checked(p);
    syrnck::NativeIdentity identity{checked(name), checked(email), offset};
    identity.validate();
    std::optional<syrnck::SigningKey> signer;
    if (key) signer.emplace(syrnck::SigningKey::from_seed(key->seed.view()));
    store.use_native_identity(std::move(identity), std::move(signer), required);
  });
}
int syrnck_store_clear_native_identity(SyrnckStore *p) {
  return boundary([&] { checked(p).clear_native_identity(); });
}
int syrnck_store_native_identity_json(SyrnckStore *p, char **out, std::size_t *length) {
  if (out) *out = nullptr;
  if (length) *length = 0;
  return boundary([&] {
    syrnck::require(out != nullptr && length != nullptr, "null identity output");
    json_output(checked(p).native_identity(), out, length, {});
  });
}
int syrnck_ingest_file(SyrnckStore *p, const char *path, char *out) {
  return syrnck_ingest_file_ex(p, path, nullptr, out);
}
int syrnck_ingest_file_ex(SyrnckStore *p, const char *path, const SyrnckChunking *options, char *out) {
  return boundary([&] {
    syrnck::require(out != nullptr, "null ID output");
    out[0] = 0;
    auto &store = checked(p);
    std::ifstream in(syrnck::path_from_utf8(checked(path)), std::ios::binary);
    syrnck::require(in.is_open(), "cannot open source file");
    const auto chunking = chunking_options(options);
    auto [id, stats] = store.ingest(in, chunking);
    (void)stats;
    store.set_ref("ffi/" + id, {syrnck::Kind::manifest, id});
    std::memcpy(out, id.c_str(), 65);
  });
}
int syrnck_file_verify(SyrnckStore *p, const char *id) {
  return boundary([&] {
    DiscardOutput output;
    std::ostream stream(&output);
    checked(p).reconstruct(checked(id), stream);
  });
}
int syrnck_restore_file(SyrnckStore *p, const char *id, const char *destination) {
  return boundary(
      [&] { checked(p).restore_file(checked(id), syrnck::path_from_utf8(checked(destination))); });
}
int syrnck_restore_file_range(SyrnckStore *p, const char *id, const char *destination, std::uint64_t offset,
                              std::uint64_t length) {
  return boundary([&] {
    checked(p).restore_file_range(checked(id), syrnck::path_from_utf8(checked(destination)), offset, length);
  });
}
int syrnck_file_read_range(SyrnckStore *p, const char *id, std::uint64_t offset, std::uint64_t requested,
                           unsigned char *out, std::size_t capacity, std::size_t *length) {
  return boundary([&] {
    syrnck::require(length != nullptr, "null size output");
    *length = 0;
    syrnck::require(requested <= syrnck::max_object_size, "buffered file range exceeds 16 MiB");
    auto &store = checked(p);
    const auto identifier = checked(id);
    const auto total = store.metadata(syrnck::Kind::manifest, identifier)["size"].get<std::uint64_t>();
    syrnck::require(offset <= total && requested <= total - offset, "file range exceeds manifest size");
    if (!out && capacity == 0) {
      *length = static_cast<std::size_t>(requested);
      return;
    }
    if (!out || capacity < requested) {
      *length = static_cast<std::size_t>(requested);
      throw syrnck::Error("insufficient output capacity");
    }
    syrnck::Bytes data(static_cast<std::size_t>(requested));
    BufferOutput buffer(data);
    std::ostream stream(&buffer);
    store.reconstruct_range(identifier, stream, offset, requested);
    copy_bytes(data, out, capacity, length);
  });
}
int syrnck_file_compare_json(SyrnckStore *p, const char *source, const char *target, char *out,
                             std::size_t capacity, std::size_t *length) {
  return boundary([&] {
    syrnck::require(length != nullptr, "null size output");
    *length = 0;
    auto value = checked(p).compare_files(checked(source), checked(target)).dump();
    copy_bytes({reinterpret_cast<const std::uint8_t *>(value.c_str()), value.size() + 1}, out, capacity,
               length);
  });
}
int syrnck_manifest_json(SyrnckStore *p, const char *id, char **out) {
  return boundary([&] {
    syrnck::require(out != nullptr, "null JSON output");
    *out = nullptr;
    auto value = checked(p).metadata(syrnck::Kind::manifest, checked(id)).dump();
    auto result = std::make_unique<char[]>(value.size() + 1);
    std::memcpy(result.get(), value.c_str(), value.size() + 1);
    *out = result.release();
  });
}
int syrnck_string_free(char *p) {
  return boundary([&] { delete[] p; });
}
int syrnck_object_has(SyrnckStore *p, const char *kind, const char *id, int *present) {
  return boundary([&] {
    syrnck::require(present != nullptr, "null presence output");
    *present = 0;
    *present = checked(p).contains(key(kind, id)) ? 1 : 0;
  });
}
int syrnck_object_read(SyrnckStore *p, const char *kind, const char *id, unsigned char *out,
                       std::size_t capacity, std::size_t *length) {
  return boundary([&] {
    syrnck::require(length != nullptr, "null size output");
    *length = 0;
    auto bytes = checked(p).get(key(kind, id));
    copy_bytes(bytes, out, capacity, length);
  });
}
int syrnck_object_import(SyrnckStore *p, const char *kind, const char *id, const unsigned char *data,
                         std::size_t length, int *created) {
  return boundary([&] {
    syrnck::require(created != nullptr, "null import output");
    *created = 0;
    syrnck::require(length <= syrnck::max_object_size, "object too large");
    syrnck::require(data != nullptr || length == 0, "null object data");
    *created = checked(p).import(key(kind, id), {data, length}) ? 1 : 0;
  });
}
int syrnck_object_put(SyrnckStore *p, const char *kind, const unsigned char *data, std::size_t length, char *out) {
  return boundary([&] {
    syrnck::require(out != nullptr, "null ID output"); out[0] = 0;
    syrnck::require(length <= syrnck::max_object_size, "object too large");
    syrnck::require(data != nullptr || length == 0, "null object data");
    auto &store = checked(p);
    const auto type = syrnck::parse_kind(checked(kind));
    const std::span<const std::uint8_t> input{data, length};
    const auto key = type == syrnck::Kind::chunk ? store.put(type, input).first
        : store.put(type, syrnck::canonical_metadata(type, input)).first;
    std::memcpy(out, key.id.c_str(), 65);
  });
}
int syrnck_prepare_native_revision_ex(SyrnckStore *p, const unsigned char *data, std::size_t length,
    const SyrnckOperation *operation, char *out) {
  return boundary([&] {
    syrnck::require(out != nullptr, "null ID output"); out[0] = 0;
    syrnck::require(length <= syrnck::max_object_size, "object too large");
    syrnck::require(data != nullptr || length == 0, "null revision draft");
    const auto control = operation_options(operation);
    syrnck::throw_if_cancelled(control.stop);
    auto draft = syrnck::parse_unique_json({data, length});
    const auto key = checked(p).prepare_native_revision(std::move(draft), control);
    std::memcpy(out, key.id.c_str(), 65);
  });
}
int syrnck_ingest_buffer_ex(SyrnckStore *p, const unsigned char *data, std::size_t length,
                             const SyrnckChunking *chunking, const SyrnckOperation *operation, char *out) {
  return boundary([&] {
    syrnck::require(out != nullptr, "null ID output"); out[0] = 0;
    syrnck::require(length <= syrnck::max_object_size, "object too large");
    syrnck::require(data != nullptr || length == 0, "null object data");
    auto &store = checked(p);
    const auto control = operation_options(operation);
    const auto configuration = chunking_options(chunking);
    BufferInput buffer({data, length}); std::istream input(&buffer);
    auto options = store.settings().ingest_options();
    options.stop = control.stop; options.progress = control.progress;
    const auto id = store.ingest(input, configuration, options).first;
    std::memcpy(out, id.c_str(), 65);
  });
}
int syrnck_prepare_file_ex(SyrnckStore *p, const char *path, const SyrnckChunking *chunking,
                            const SyrnckOperation *operation, char *out) {
  return boundary([&] {
    syrnck::require(out != nullptr, "null ID output"); out[0] = 0;
    auto &store = checked(p);
    const auto control = operation_options(operation);
    const auto configuration = chunking_options(chunking);
    std::ifstream input(syrnck::path_from_utf8(checked(path)), std::ios::binary);
    syrnck::require(input.is_open(), "cannot open source file");
    auto options = store.settings().ingest_options();
    options.stop = control.stop; options.progress = control.progress;
    const auto id = store.ingest(input, configuration, options).first;
    std::memcpy(out, id.c_str(), 65);
  });
}
int syrnck_refs_json(SyrnckStore *p, char *out, std::size_t capacity, std::size_t *length) {
  return boundary([&] {
    syrnck::require(length != nullptr, "null size output"); *length = 0;
    const auto value = syrnck::encode_refs(checked(p).refs()).dump();
    copy_bytes({reinterpret_cast<const std::uint8_t *>(value.c_str()), value.size() + 1}, out, capacity, length);
  });
}
int syrnck_refs_compare_json(SyrnckStore *p, const unsigned char *data, std::size_t length,
                             const SyrnckOperation *operation, int *applied) {
  return boundary([&] {
    syrnck::require(applied != nullptr, "null reference transaction output"); *applied = 0;
    syrnck::require(length <= syrnck::max_object_size, "object too large");
    syrnck::require(data != nullptr || length == 0, "null object data");
    auto &store = checked(p);
    const auto control = operation_options(operation);
    syrnck::throw_if_cancelled(control.stop);
    const auto document = syrnck::parse_unique_json({data, length});
    syrnck::require(document.is_array() && document.size() <= 65536, "invalid reference transaction document");
    std::vector<syrnck::RefUpdate> updates; updates.reserve(document.size());
    const auto typed = [](const syrnck::Json &value) -> std::optional<syrnck::ObjectKey> {
      return value.is_null() ? std::optional<syrnck::ObjectKey>{} : syrnck::parse_key(value);
    };
    for (const auto &row : document) {
      syrnck::throw_if_cancelled(control.stop);
      syrnck::require(row.is_object() && row.size() == 3 && row.contains("name") && row["name"].is_string() &&
          row.contains("expected") && row.contains("value"), "invalid reference transaction document");
      updates.push_back({row["name"].get<std::string>(), typed(row["expected"]), typed(row["value"])});
    }
    *applied = store.compare_update_refs(updates, control) ? 1 : 0;
  });
}
int syrnck_closure_json(SyrnckStore *p, const char *kind, const char *id, char *out, std::size_t capacity,
                        std::size_t *length) {
  return boundary([&] {
    syrnck::require(length != nullptr, "null size output");
    *length = 0;
    auto keys = checked(p).closure({key(kind, id)});
    auto value = syrnck::Json::array();
    for (const auto &item : keys)
      value.push_back(syrnck::key_json(item));
    auto bytes = value.dump();
    copy_bytes({reinterpret_cast<const std::uint8_t *>(bytes.c_str()), bytes.size() + 1}, out, capacity,
               length);
  });
}
int syrnck_ref_set(SyrnckStore *p, const char *name, const char *kind, const char *id) {
  return boundary([&] { checked(p).set_ref(checked(name), key(kind, id)); });
}
int syrnck_ref_remove(SyrnckStore *p, const char *name) {
  return boundary([&] { checked(p).remove_ref(checked(name)); });
}
int syrnck_store_verify(SyrnckStore *p, std::size_t *objects) {
  return boundary([&] {
    syrnck::require(objects != nullptr, "null verification output");
    *objects = 0;
    *objects = checked(p).fsck();
  });
}
int syrnck_cancellation_create(SyrnckCancellation **out) {
  return boundary([&] {
    syrnck::require(out != nullptr, "null output handle");
    *out = nullptr;
    *out = new SyrnckCancellation;
  });
}
int syrnck_cancellation_request(SyrnckCancellation *cancellation) {
  return boundary([&] {
    syrnck::require(cancellation != nullptr, "null cancellation handle");
    cancellation->source.request_stop();
  });
}
int syrnck_cancellation_close(SyrnckCancellation *cancellation) {
  return boundary([&] { delete cancellation; });
}
int syrnck_workspace_open(const char *directory, const char *storage, const SyrnckWorkspaceOptions *options,
                           SyrnckWorkspace **out) {
  return syrnck_workspace_open_ex(directory, storage, options, nullptr, out);
}
int syrnck_workspace_open_ex(const char *directory, const char *storage, const SyrnckWorkspaceOptions *options,
                              const SyrnckCancellation *cancellation, SyrnckWorkspace **out) {
  return syrnck_workspace_open_registered_ex(directory, storage, nullptr, options, cancellation, out);
}
int syrnck_workspace_open_registered_ex(const char *directory, const char *storage, const char *name,
                              const SyrnckWorkspaceOptions *options,
                              const SyrnckCancellation *cancellation, SyrnckWorkspace **out) {
  return boundary([&] {
    syrnck::require(out != nullptr, "null output handle");
    *out = nullptr;
    const auto stop = stop_token(cancellation);
    syrnck::throw_if_cancelled(stop);
    syrnck::WorkspaceWatchOptions selected;
    const std::string context = name ? name : "";
    if (!context.empty()) syrnck::validate_native_workspace_name(context);
    if (options) {
      syrnck::require(options->polling <= 1, "invalid workspace polling flag");
      selected = {options->polling != 0, options->max_paths, options->max_directories};
    }
    selected.external_store = !context.empty();
    auto result = std::make_unique<SyrnckWorkspace>(syrnck::path_from_utf8(checked(directory)),
                                                    syrnck::path_from_utf8(checked(storage)), selected, stop, context);
    syrnck::throw_if_cancelled(stop);
    *out = result.release();
  });
}
int syrnck_workspace_close(SyrnckWorkspace *workspace) {
  return boundary([&] { delete workspace; });
}
int syrnck_workspace_wait_json(SyrnckWorkspace *workspace, std::uint32_t timeout, char **out,
                               std::size_t *length) {
  return syrnck_workspace_wait_json_ex(workspace, timeout, nullptr, out, length);
}
int syrnck_workspace_wait_json_ex(SyrnckWorkspace *workspace, std::uint32_t timeout,
                                  const SyrnckCancellation *cancellation, char **out, std::size_t *length) {
  return workspace_boundary(workspace, out, length, [&](SyrnckWorkspace &session) {
    const auto stop = stop_token(cancellation);
    auto changes = session.watcher.wait(std::chrono::milliseconds(timeout), stop);
    session.pending.merge(changes, session.max_paths);
    json_output(syrnck::Json{{"version", 1}, {"backend", session.watcher.backend()}, {"paths", changes.paths},
                  {"full_scan", changes.full_scan}, {"overflow", changes.overflow}, {"polling", changes.polling}},
                out, length, stop);
  });
}
int syrnck_workspace_status_json(SyrnckWorkspace *workspace, SyrnckStore *p, const char *selector,
                                 std::uint32_t mode, char **out, std::size_t *length) {
  return syrnck_workspace_status_json_ex(workspace, p, selector, mode, nullptr, out, length);
}
int syrnck_workspace_status_json_ex(SyrnckWorkspace *workspace, SyrnckStore *p, const char *selector,
                                    std::uint32_t mode, const SyrnckOperation *operation, char **out,
                                    std::size_t *length) {
  return workspace_boundary(workspace, out, length, [&](SyrnckWorkspace &session) {
    const auto options = operation_options(operation);
    syrnck::throw_if_cancelled(options.stop);
    auto &store = workspace_store(session, p);
    syrnck::require(mode <= 2, "invalid workspace status mode");
    // Identity checks and newly queued hints apply even without an explicit wait.
    session.pending.merge(session.watcher.wait(std::chrono::milliseconds(0), options.stop), session.max_paths);
    syrnck::require_no_native_checkout(store);
    syrnck::Json report;
    if (mode == 1) {
      const auto revision = store.resolve_native_revision(checked(selector), options);
      report = session.observation.status(store, revision, session.pending, options);
    } else {
      const auto branch = selector ? std::string(selector) : store.native_target();
      syrnck::validate_native_target(branch);
      if (mode == 2) report = session.observation.workspace_status(store, branch, session.pending, options);
      else {
        std::optional<std::string> revision;
        const auto refs = store.refs();
        if (const auto found = refs.find(syrnck::native_revision_reference(store, branch)); found != refs.end()) {
          syrnck::require(found->second.kind == syrnck::Kind::revision, "branch must point to a revision");
          revision = found->second.id;
        }
        report = session.observation.status(store, revision, session.pending, options);
      }
    }
    json_output(report, out, length, options.stop);
    session.pending = {};
  });
}
int syrnck_workspace_invalidate(SyrnckWorkspace *workspace) {
  return boundary([&] { checked(workspace).invalidate(); });
}
int syrnck_workspace_native_state_json_ex(SyrnckWorkspace *workspace, SyrnckStore *handle,
    const SyrnckOperation *operation, char **out, std::size_t *length) {
  return workspace_boundary(workspace, out, length, [&](SyrnckWorkspace &session) {
    const auto options = operation_options(operation);
    syrnck::throw_if_cancelled(options.stop);
    auto &store = workspace_store(session, handle);
    const auto target = store.native_target();
    const auto refs = store.refs();
    const auto reference = syrnck::native_revision_reference(store, target);
    const auto at = refs.find(reference);
    syrnck::Json revision;
    if (at != refs.end()) {
      syrnck::require(at->second.kind == syrnck::Kind::revision, "branch must point to a revision");
      revision = syrnck::Json{{"kind", "revision"}, {"id", at->second.id}};
    }
    json_output(syrnck::Json{{"version", 1}, {"workspace", session.context}, {"target", target},
        {"reference", reference}, {"revision", revision}, {"head", store.native_head()},
        {"materialization", store.native_materialization()}, {"checkout", store.checkout_status()},
        {"staging", refs.contains(syrnck::native_staging_reference(store, target))}}, out, length, options.stop);
  });
}
int syrnck_workspace_checkout_status_json_ex(SyrnckWorkspace *workspace, SyrnckStore *handle,
    const char *transaction, const SyrnckOperation *operation, char **out, std::size_t *length) {
  return workspace_boundary(workspace, out, length, [&](SyrnckWorkspace &session) {
    const auto options = operation_options(operation);
    syrnck::throw_if_cancelled(options.stop);
    auto &store = workspace_store(session, handle);
    const auto id = checked(transaction); syrnck::validate_id(id);
    const auto refs = store.refs();
    const auto receipt = refs.find(store.native_workspace_reference("checkouts/receipts/" + std::string(id)));
    syrnck::Json report;
    if (receipt == refs.end()) report = store.checkout_status(std::string(id));
    else {
      // Completed evidence does not need to diff all files in two historical
      // trees. Keep receipt lookup proportional to metadata, not asset count.
      report = syrnck::read_native_checkout_receipt(store, receipt->second, id);
      const auto target = refs.find(store.native_workspace_reference("checkouts/completed/" + std::string(id)));
      syrnck::require(target != refs.end() && target->second.kind == syrnck::Kind::revision &&
          target->second.id == report.at("target").get<std::string>(), "native checkout receipt and target disagree");
      report["pending"] = false;
    }
    syrnck::require(report.value("context", "") == session.context &&
        syrnck::path_from_utf8(report.at("directory").get<std::string>()) == session.directory,
        "native checkout receipt belongs to another workspace");
    json_output(report, out, length, options.stop);
  });
}
int syrnck_workspace_capture_json_ex(SyrnckWorkspace *workspace, SyrnckStore *handle,
    const SyrnckNativeExpected *expected, const char *message, const SyrnckOperation *operation,
    char **out, std::size_t *length) {
  return workspace_boundary(workspace, out, length, [&](SyrnckWorkspace &session) {
    const auto options = operation_options(operation);
    syrnck::throw_if_cancelled(options.stop);
    auto &store = workspace_store(session, handle);
    const auto target = expected_native_target(store, expected);
    const auto text = checked(message);
    syrnck::require(std::strlen(text) <= 65536, "native capture message exceeds 64 KiB");
    auto ingest = store.settings().ingest_options();
    ingest.stop = options.stop; ingest.progress = options.progress;
    // Consume pre-capture hints before the complete inventory; notifications
    // arriving during/after capture remain queued for the next observation.
    session.pending.merge(session.watcher.wait(std::chrono::milliseconds(0), options.stop), session.max_paths);
    session.observation.invalidate();
    const auto report = session.capture.capture(store, target, text, ingest, &session.observation);
    // A completed native publication wins late cancellation. Output allocation
    // can still fail: callers must reload state on every nonzero status.
    json_output(report, out, length, {});
    session.pending = {};
  });
}
int syrnck_workspace_checkout_plan_json_ex(SyrnckWorkspace *workspace, SyrnckStore *handle,
    const SyrnckNativeExpected *expected, const char *revision, const SyrnckOperation *operation,
    char **out, std::size_t *length) {
  return workspace_boundary(workspace, out, length, [&](SyrnckWorkspace &session) {
    const auto options = operation_options(operation);
    syrnck::throw_if_cancelled(options.stop);
    auto &store = workspace_store(session, handle);
    const auto target = expected_native_target(store, expected);
    const auto id = checked(revision); syrnck::validate_id(id);
    json_output(store.plan_checkout(session.directory, target, id, true, options), out, length, options.stop);
  });
}
int syrnck_workspace_checkout_owned_json_ex(SyrnckWorkspace *workspace, SyrnckStore *handle,
    const SyrnckNativeExpected *expected, const char *revision, const char *owner,
    const SyrnckOperation *operation, char **out, std::size_t *length) {
  return workspace_boundary(workspace, out, length, [&](SyrnckWorkspace &session) {
    const auto options = operation_options(operation);
    syrnck::throw_if_cancelled(options.stop);
    auto &store = workspace_store(session, handle);
    const auto target = expected_native_target(store, expected);
    const auto id = checked(revision), identity = checked(owner);
    syrnck::validate_id(id); syrnck::validate_id(identity);
    session.invalidate();
    const auto report = store.checkout_directory_owned(session.directory, target, id, identity, options);
    json_output(report, out, length, {});
  });
}
static int workspace_recover_owned_impl(SyrnckWorkspace *workspace, SyrnckStore *handle,
    const char *target, const char *revision, const char *owner, const SyrnckOperation *operation,
    char **out, std::size_t *length, bool preserve) {
  return workspace_boundary(workspace, out, length, [&](SyrnckWorkspace &session) {
    const auto options = operation_options(operation);
    syrnck::throw_if_cancelled(options.stop);
    auto &store = workspace_store(session, handle);
    const auto selected = checked(target), id = checked(revision), identity = checked(owner);
    syrnck::validate_native_target(selected); syrnck::validate_id(id); syrnck::validate_id(identity);
    syrnck::require(store.native_target() == selected, "native workspace HEAD selection changed");
    const auto pending = store.checkout_status();
    syrnck::require(pending["pending"].get<bool>() && pending.value("owner", "") == identity &&
        pending["target"] == id, "native owned recovery target or owner does not match");
    session.invalidate();
    const auto report = preserve ? store.recover_checkout_owned_preserving_files(session.directory, selected, identity, options)
                                : store.recover_checkout_owned(session.directory, selected, identity, options);
    json_output(report, out, length, {});
  });
}
int syrnck_workspace_recover_owned_json_ex(SyrnckWorkspace *workspace, SyrnckStore *handle,
    const char *target, const char *revision, const char *owner, const SyrnckOperation *operation,
    char **out, std::size_t *length) {
  return workspace_recover_owned_impl(workspace, handle, target, revision, owner, operation, out, length, false);
}
int syrnck_workspace_recover_owned_preserving_json_ex(SyrnckWorkspace *workspace, SyrnckStore *handle,
    const char *target, const char *revision, const char *owner, const SyrnckOperation *operation,
    char **out, std::size_t *length) {
  return workspace_recover_owned_impl(workspace, handle, target, revision, owner, operation, out, length, true);
}

}
