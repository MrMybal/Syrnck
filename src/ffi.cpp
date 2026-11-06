#include <cstring>
#include <fstream>
#include <algorithm>
#include <streambuf>
#include <syrnck.h>
#include <syrnck/core.hpp>
#include <syrnck/watcher.hpp>
#include "native_workspace.hpp"
struct SyrnckStore {
  syrnck::Store store;
  SyrnckStore(const syrnck::fs::path &p, std::optional<syrnck::Key> key, bool create)
      : store(p, key, create) {}
};
struct SyrnckWorkspace {
  syrnck::fs::path directory;
  syrnck::WorkspaceWatcher watcher;
  syrnck::WorkspaceObservation observation;
  syrnck::WorkspaceChanges pending;
  std::size_t max_paths;
  SyrnckWorkspace(const syrnck::fs::path &root, const syrnck::fs::path &storage,
                  syrnck::WorkspaceWatchOptions options)
      : directory(syrnck::fs::canonical(root)), watcher(directory, storage, options),
        observation(directory, storage), max_paths(options.max_paths) { invalidate(); }
  void invalidate() {
    observation.invalidate();
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
syrnck::Store &checked(SyrnckStore *p) {
  syrnck::require(p != nullptr, "null store handle");
  return p->store;
}
SyrnckWorkspace &checked(SyrnckWorkspace *p) {
  syrnck::require(p != nullptr, "null workspace handle");
  return *p;
}
void json_output(const syrnck::Json &value, char **out, std::size_t *length) {
  auto text = value.dump();
  syrnck::require(text.size() <= 128 * 1024 * 1024, "workspace output exceeds 128 MiB");
  auto result = std::make_unique<char[]>(text.size() + 1);
  std::memcpy(result.get(), text.c_str(), text.size() + 1);
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
    if (length) {
      syrnck::require(length == 32 && key != nullptr, "key_len must be 0 or 32 with a valid key");
      secret.emplace();
      std::copy_n(key, 32, secret->begin());
    }
    *out = new SyrnckStore(syrnck::path_from_utf8(checked(path)), secret, create);
  });
}
int syrnck_store_close(SyrnckStore *store) {
  return boundary([&] { delete store; });
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
    syrnck::Chunking chunking;
    if (options) {
      syrnck::require(options->algorithm <= 1, "unknown chunking algorithm");
      chunking.fixed = options->algorithm == 1;
      chunking.min = options->min;
      chunking.avg = options->average;
      chunking.max = options->max;
      chunking.validate();
      if (chunking.fixed)
        syrnck::require(chunking.min == chunking.avg && chunking.avg == chunking.max,
                        "fixed chunking sizes must match");
    }
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
int syrnck_workspace_open(const char *directory, const char *storage, const SyrnckWorkspaceOptions *options,
                           SyrnckWorkspace **out) {
  return boundary([&] {
    syrnck::require(out != nullptr, "null output handle");
    *out = nullptr;
    syrnck::WorkspaceWatchOptions selected;
    if (options) {
      syrnck::require(options->polling <= 1, "invalid workspace polling flag");
      selected = {options->polling != 0, options->max_paths, options->max_directories};
    }
    *out = new SyrnckWorkspace(syrnck::path_from_utf8(checked(directory)),
                                syrnck::path_from_utf8(checked(storage)), selected);
  });
}
int syrnck_workspace_close(SyrnckWorkspace *workspace) {
  return boundary([&] { delete workspace; });
}
int syrnck_workspace_wait_json(SyrnckWorkspace *workspace, std::uint32_t timeout, char **out,
                               std::size_t *length) {
  return workspace_boundary(workspace, out, length, [&](SyrnckWorkspace &session) {
    auto changes = session.watcher.wait(std::chrono::milliseconds(timeout));
    session.pending.merge(changes, session.max_paths);
    json_output(syrnck::Json{{"version", 1}, {"backend", session.watcher.backend()}, {"paths", changes.paths},
                  {"full_scan", changes.full_scan}, {"overflow", changes.overflow}, {"polling", changes.polling}},
                out, length);
  });
}
int syrnck_workspace_status_json(SyrnckWorkspace *workspace, SyrnckStore *p, const char *selector,
                                 std::uint32_t mode, char **out, std::size_t *length) {
  return workspace_boundary(workspace, out, length, [&](SyrnckWorkspace &session) {
    auto &store = checked(p);
    syrnck::require(mode <= 2, "invalid workspace status mode");
    // Identity checks and newly queued hints apply even without an explicit wait.
    session.pending.merge(session.watcher.wait(std::chrono::milliseconds(0)), session.max_paths);
    const auto head = store.native_head();
    syrnck::require(head["directory"].is_null() || head["directory"].get<std::string>() == syrnck::utf8(session.directory),
                    "native HEAD belongs to another workspace");
    syrnck::require_no_native_checkout(store);
    syrnck::Json report;
    if (mode == 1) {
      const auto revision = store.resolve_native_revision(checked(selector));
      report = session.observation.status(store, revision, session.pending);
    } else {
      const auto branch = selector ? std::string(selector) : head["branch"].get<std::string>();
      syrnck::validate_ref(branch);
      if (mode == 2) report = session.observation.workspace_status(store, branch, session.pending);
      else {
        std::optional<std::string> revision;
        const auto refs = store.refs();
        if (const auto found = refs.find("heads/" + branch); found != refs.end()) {
          syrnck::require(found->second.kind == syrnck::Kind::revision, "branch must point to a revision");
          revision = found->second.id;
        }
        report = session.observation.status(store, revision, session.pending);
      }
    }
    json_output(report, out, length);
    session.pending = {};
  });
}
int syrnck_workspace_invalidate(SyrnckWorkspace *workspace) {
  return boundary([&] { checked(workspace).invalidate(); });
}
}
