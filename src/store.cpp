#include "internal.hpp"
#include "io_checkpoint.hpp"
#include "encode_pool.hpp"
#include "object_cache.hpp"
#include "pack_storage.hpp"
#include "object_marks.hpp"
#include "watch_journal.hpp"
#include "native_workspace.hpp"
#include "native_context.hpp"
#include "native_materialization.hpp"
#include "native_workspace_relocation.hpp"
#include "native_authorship.hpp"
#include <algorithm>
#include <blake3.h>
#include <fstream>
#include <cstring>
#include <limits>
#include <monocypher.h>
#include <streambuf>
#include <zstd.h>
namespace syrnck {
struct Store::Impl {
  fs::path root;
  std::string native_workspace;
  std::optional<NativeIdentity> native_identity;
  std::optional<SigningKey> native_signer;
  std::optional<Key> key;
  FileLock lock;
  PackStorage packs;
  ObjectCache cache;
  StoreIoStats io;
  bool bypass_cache = false;
  int format_version = 1;
  std::vector<std::string> features;
  std::size_t index_cache_budget;
  Impl(fs::path p, std::optional<Key> k, StoreOptions options)
      : root(std::move(p)), key(k), lock(root / "store.lock"), packs(root, options.pack_index_cache_bytes),
        cache(options), index_cache_budget(options.pack_index_cache_bytes) {}
  Json load_config();
  void publish_pack(PackStorage next, std::stop_token = {});
  Bytes read_pack(const PackStorage &source, const ObjectKey &item) {
    auto envelope = source.read(item);
    ++io.object_reads;
    io.envelope_bytes_read += envelope.size();
    return envelope;
  }
  ~Impl() {
    if (key)
      crypto_wipe(key->data(), key->size());
  }
};
static std::string key_check(const Key &key) {
  blake3_hasher h;
  blake3_hasher_init_keyed(&h, key.data());
  std::string_view msg = "syrnck-store-key-check-v1";
  blake3_hasher_update(&h, msg.data(), msg.size());
  Key out{};
  blake3_hasher_finalize(&h, out.data(), out.size());
  return hex(out);
}
static void supported_feature(std::string_view feature) {
  require(feature == "linked-workspaces-v1" || feature == "sparse-workspaces-v1" ||
              feature == "materialization-transitions-v1" || feature == "workspace-relocation-v1" ||
              feature == "authored-revisions-v1" || feature == "resident-sync-v1", "unsupported required store feature");
}
Json Store::Impl::load_config() {
  packs.invalidate();
  auto config = parse_json(read_bytes(root / "config.json"));
  require(config.is_object(), "invalid store configuration");
  auto version = config.at("version");
  const auto additional = config.contains("required_features") ? 1U : 0U;
  require((version == 1 && config.size() == 2 + additional) ||
              ((version == 2 || version == 3) && config.size() == 3 + additional),
          "unsupported store configuration");
  std::vector<std::string> required;
  if (additional) {
    const auto &value = config.at("required_features");
    require(value.is_array() && !value.empty() && value.size() <= 32, "invalid required store features");
    for (const auto &entry : value) {
      require(entry.is_string(), "invalid required store features");
      const auto feature = entry.get<std::string>();
      supported_feature(feature);
      require(required.empty() || required.back() < feature, "invalid required store features");
      required.push_back(feature);
    }
  }
  auto expected = key ? Json(key_check(*key)) : Json(nullptr);
  require(config.at("key_check") == expected,
          "store encryption key missing, wrong, or supplied for an unencrypted store");
  if (version == 2)
    require(config.at("pack").is_null() || config.at("pack").is_object(), "invalid v2 pack descriptor");
  if (version == 3)
    require(config.at("packs").is_array(), "invalid v3 pack catalog");
  packs.load(version == 3 ? config.at("packs") : (version == 2 ? config.at("pack") : Json(nullptr)));
  format_version = version.get<int>();
  features = std::move(required);
  return config;
}
void Store::Impl::publish_pack(PackStorage next, std::stop_token stop) {
  next.set_index_cache_limit(index_cache_budget);
  const bool multiple = format_version >= 3 || next.pack_count() > 1;
  auto document = Json{{"version", multiple ? 3 : 2},
                     {"key_check", key ? Json(key_check(*key)) : Json(nullptr)},
                     {multiple ? "packs" : "pack", multiple ? next.catalog() : next.metadata()}};
  if (!features.empty()) document["required_features"] = features;
  const auto config = document.dump();
  try {
    atomic_write(root / "config.json", bytes(config), true, stop);
  } catch (...) {
    // A durability error can happen after the atomic replacement. Reload the
    // actual publication pointer before allowing any more operations or cleanup.
    load_config();
    throw;
  }
  packs = std::move(next);
  format_version = multiple ? 3 : 2;
}
Store::Store(const fs::path &root, std::optional<Key> key, bool create, StoreOptions options) {
  require(options.decoded_cache_bytes <= 1024ULL * 1024 * 1024 &&
              options.verified_cache_entries <= 1024 * 1024 &&
              options.pack_index_cache_bytes <= 1024ULL * 1024 * 1024,
          "store cache limits are too large");
  if (create)
    fs::create_directories(root);
  else
    require(fs::is_regular_file(root / "config.json"), "not a Syrnck store; initialize it first");
  impl_ = std::make_unique<Impl>(fs::canonical(root), key, options);
  if (key)
    crypto_wipe(key->data(), key->size());
  if (create) {
    for (const auto &entry : fs::directory_iterator(impl_->root))
      require(entry.path().filename() == "store.lock", "initialization requires a new or empty directory");
    atomic_write(impl_->root / "refs.json", bytes("{}"), false);
    fs::create_directories(impl_->root / "objects");
    auto config =
        Json{{"version", 1}, {"key_check", impl_->key ? Json(key_check(*impl_->key)) : Json(nullptr)}}.dump();
    atomic_write(impl_->root / "config.json", bytes(config), false);
  } else {
    impl_->load_config();
    validate_native_materialization_requirements(*this, refs());
  }
}
Store::~Store() = default;
Store::Store(Store &&) noexcept = default;
Store &Store::operator=(Store &&) noexcept = default;
const fs::path &Store::root() const {
  return impl_->root;
}
bool Store::encrypted() const {
  return impl_->key.has_value();
}
std::vector<std::string> Store::required_features() const { return impl_->features; }
void Store::require_features(const std::vector<std::string> &features, OperationOptions options) const {
  throw_if_cancelled(options.stop);
  require(features.size() <= 32, "invalid required store features");
  for (const auto &feature : features) supported_feature(feature);
  if (features.empty()) return;
  if (std::all_of(features.begin(), features.end(), [&](const auto &feature) {
        return std::binary_search(impl_->features.begin(), impl_->features.end(), feature);
      })) return;
  auto config = impl_->load_config();
  auto required = impl_->features;
  required.insert(required.end(), features.begin(), features.end());
  std::sort(required.begin(), required.end());
  required.erase(std::unique(required.begin(), required.end()), required.end());
  if (required == impl_->features) return;
  const auto path = root() / "config.json";
  const auto original = read_bytes(path);
  require(parse_json(original) == config, "store configuration changed; retry operation");
  config["required_features"] = required;
  report_progress(options, {ProgressPhase::publishing, "store-features"});
  require(read_bytes(path) == original, "store configuration changed; retry operation");
  try {
    atomic_write(path, bytes(config.dump()), true, options.stop);
  } catch (...) {
    impl_->load_config();
    throw;
  }
  impl_->load_config();
}
StoreIoStats Store::io_stats() const {
  auto result = impl_->io;
  result.cached_objects = impl_->cache.entries();
  result.cached_payloads = impl_->cache.payloads();
  result.cached_bytes = impl_->cache.bytes();
  auto index = impl_->packs.index_stats();
  result.index_directory_bytes = index.directory_bytes;
  result.index_filter_bytes = index.filter_bytes;
  result.index_cached_bytes = index.cache_bytes;
  result.index_cached_pages = index.cached_pages;
  return result;
}
void Store::reset_io_stats() const {
  impl_->io = {};
}
void Store::clear_cache() const {
  impl_->cache.clear();
  impl_->packs.clear_index_cache();
}
fs::path Store::object_path(const ObjectKey &key) const {
  validate_id(key.id);
  return root() / "objects" / kind_name(key.kind) / key.id.substr(0, 2) / key.id;
}
bool Store::contains(const ObjectKey &key) const {
  const bool packed = impl_->packs.contains(key);
  if (!fs::exists(object_path(key)) && !packed) {
    impl_->cache.erase(key);
    return false;
  }
  (void)load_object(key, true);
  return true;
}
static Bytes encode_object(const ObjectKey &key, std::span<const std::uint8_t> data,
                           const std::optional<Key> &secret, ZSTD_CCtx *context = nullptr) {
  Bytes compressed(ZSTD_compressBound(data.size()));
  auto n = context
               ? ZSTD_compressCCtx(context, compressed.data(), compressed.size(), data.data(), data.size(), 3)
               : ZSTD_compress(compressed.data(), compressed.size(), data.data(), data.size(), 3);
  require(!ZSTD_isError(n), "compression failed");
  Bytes envelope(n + (secret ? 45 : 5));
  std::memcpy(envelope.data(), "SYR1", 4);
  envelope[4] = static_cast<std::uint8_t>(secret ? 1 : 0);
  if (secret) {
    auto nonce = random_bytes(24);
    auto aad = "syrnck-envelope-v1:" + std::string(kind_name(key.kind)) + ":" + key.id;
    std::array<std::uint8_t, 16> mac{};
    std::copy(nonce.begin(), nonce.end(), envelope.begin() + 5);
    crypto_aead_lock(envelope.data() + 29, mac.data(), secret->data(), nonce.data(),
                     reinterpret_cast<const std::uint8_t *>(aad.data()), aad.size(), compressed.data(), n);
    std::copy(mac.begin(), mac.end(), envelope.begin() + static_cast<std::ptrdiff_t>(29 + n));
  } else
    std::copy_n(compressed.begin(), n, envelope.begin() + 5);
  return envelope;
}
namespace {
std::optional<Bytes> watch_journal_bytes(const Store &store) {
  const auto path = native_workspace_local_path(store, "watch-journal");
  const auto status = fs::symlink_status(path);
  if (!fs::exists(status)) return {};
  require(fs::is_regular_file(status) && !native_link_like(path, status), "invalid workspace watch journal file");
  return read_bytes(path, watch_journal_limit + 1024 * 1024);
}
} // namespace
Json Store::workspace_watch_journal(OperationOptions options) const {
  throw_if_cancelled(options.stop);
  const auto original = watch_journal_bytes(*this);
  if (!original) return {};
  require(original->size() >= 73 && std::equal(original->begin(), original->begin() + 4, "SYW1"),
          "invalid workspace watch journal file");
  const auto id = text(std::span(*original).subspan(4, 64));
  validate_id(id);
  const auto decoded = decode_envelope({Kind::chunk, id}, std::span(*original).subspan(68));
  require(decoded.size() <= watch_journal_limit, "workspace watch journal exceeds 2 MiB");
  const auto value = parse_json(decoded);
  validate_watch_journal(value, options);
  require(value.dump() == text(decoded), "invalid workspace watch journal");
  return value;
}
void Store::write_workspace_watch_journal(const Json &value, OperationOptions options) const {
  validate_watch_journal(value, options);
  const auto original = watch_journal_bytes(*this);
  const auto data = value.dump();
  const auto id = object_id(Kind::chunk, bytes(data));
  auto document = Bytes{'S', 'Y', 'W', '1'};
  document.insert(document.end(), id.begin(), id.end());
  const auto envelope = encode_object({Kind::chunk, id}, bytes(data), impl_->key);
  document.insert(document.end(), envelope.begin(), envelope.end());
  report_progress(options, {ProgressPhase::publishing, "watch-journal", 0, 1});
  require(watch_journal_bytes(*this) == original, "workspace watch journal changed; retry operation");
  atomic_write(native_workspace_local_path(*this, "watch-journal"), document, true, options.stop);
}
Json Store::clear_workspace_watch_journal(OperationOptions options) const {
  throw_if_cancelled(options.stop);
  auto lock = lock_watch_journal(native_workspace_local_path(*this, "watch.lock").parent_path());
  const auto original = watch_journal_bytes(*this);
  report_progress(options, {ProgressPhase::publishing, "watch-journal", 0, 1});
  require(watch_journal_bytes(*this) == original, "workspace watch journal changed; retry operation");
  if (original) {
    require(fs::remove(native_workspace_local_path(*this, "watch-journal")), "cannot clear workspace watch journal");
    sync_directory(native_workspace_local_path(*this, "watch-journal").parent_path());
  }
  return Json{{"version", 1}, {"state", original ? "cleared" : "absent"}};
}
std::pair<ObjectKey, bool> Store::put(Kind kind, std::span<const std::uint8_t> data) const {
  validate_object(kind, data);
  // Canonical metadata always begins with this version field. Gate even
  // unreferenced v2 objects: historical readers also audit loose metadata.
  const auto encoded = std::string_view(reinterpret_cast<const char *>(data.data()), data.size());
  if (kind == Kind::revision && encoded.starts_with("{\"version\":2,"))
    require_features({"authored-revisions-v1"});
  ObjectKey key{kind, object_id(kind, data)};
  if (contains(key))
    return {key, false};
  atomic_write(object_path(key), encode_object(key, data, impl_->key), false);
  return {key, true};
}
bool Store::import(const ObjectKey &key, std::span<const std::uint8_t> data) const {
  validate_id(key.id);
  require(object_id(key.kind, data) == key.id, "object hash mismatch");
  return put(key.kind, data).second;
}
Bytes Store::get(const ObjectKey &key) const {
  return load_object(key, false);
}
Bytes Store::read_envelope(const ObjectKey &key) const {
  const bool packed = impl_->packs.contains(key);
  Bytes envelope;
  if (fs::exists(object_path(key)))
    envelope = read_bytes(object_path(key));
  else if (packed)
    return impl_->read_pack(impl_->packs, key);
  else {
    impl_->cache.erase(key);
    throw MissingObject("object not found: " + key.id);
  }
  ++impl_->io.object_reads;
  impl_->io.envelope_bytes_read += envelope.size();
  return envelope;
}
Bytes Store::load_object(const ObjectKey &key, bool presence_only) const {
  auto envelope = read_envelope(key);
  const bool use_cache = !impl_->bypass_cache && impl_->cache.enabled();
  Key fingerprint{};
  if (use_cache) {
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, envelope.data(), envelope.size());
    blake3_hasher_finalize(&hasher, fingerprint.data(), fingerprint.size());
    auto hit = impl_->cache.lookup(key, fingerprint);
    if (hit.verified && presence_only) {
      ++impl_->io.cache_presence_hits;
      return {};
    }
    if (hit.payload) {
      ++impl_->io.cache_data_hits;
      return *hit.payload;
    }
  }
  auto data = decode_envelope(key, envelope);
  if (use_cache)
    impl_->cache.remember(key, fingerprint, data);
  return data;
}
Bytes Store::decode_envelope(const ObjectKey &key, std::span<const std::uint8_t> envelope) const {
  require(envelope.size() >= 5 && std::equal(envelope.begin(), envelope.begin() + 4, "SYR1"),
          "invalid object envelope");
  Bytes compressed;
  if (envelope[4] == 0 && !encrypted())
    compressed.assign(envelope.begin() + 5, envelope.end());
  else if (envelope[4] == 1 && impl_->key && envelope.size() >= 45) {
    auto size = envelope.size() - 45;
    compressed.resize(size);
    auto aad = "syrnck-envelope-v1:" + std::string(kind_name(key.kind)) + ":" + key.id;
    require(crypto_aead_unlock(compressed.data(), envelope.data() + 29 + size, impl_->key->data(),
                               envelope.data() + 5, reinterpret_cast<const std::uint8_t *>(aad.data()),
                               aad.size(), envelope.data() + 29, size) == 0,
            "object authentication failed");
  } else
    throw Error("object encryption mode mismatch or truncated envelope");
  // Bound decompression even if a malicious frame advertises an enormous size.
  auto expected = ZSTD_getFrameContentSize(compressed.data(), compressed.size());
  require(expected != ZSTD_CONTENTSIZE_ERROR &&
              (expected == ZSTD_CONTENTSIZE_UNKNOWN || expected <= max_object_size),
          "invalid or oversized compressed object");
  Bytes data(expected == ZSTD_CONTENTSIZE_UNKNOWN ? max_object_size : static_cast<std::size_t>(expected));
  auto n = ZSTD_decompress(data.data(), data.size(), compressed.data(), compressed.size());
  require(!ZSTD_isError(n), "invalid or oversized compressed object");
  data.resize(n);
  require(object_id(key.kind, data) == key.id, "stored object hash mismatch");
  validate_object(key.kind, data);
  ++impl_->io.decoded_objects;
  return data;
}
Json Store::metadata(Kind kind, std::string_view id) const {
  require(kind != Kind::chunk, "chunk is not metadata");
  return parse_json(get({kind, std::string(id)}));
}
std::pair<std::string, IngestStats> Store::ingest(std::istream &source, Chunking chunking,
                                                  IngestOptions options) const {
  auto session = impl_->packs.read_session();
  chunking.validate();
  require(options.workers >= 1 && options.workers <= 64, "ingest workers must be in 1..64");
  require(options.max_in_flight_bytes >= chunking.max && options.max_in_flight_bytes <= 1024ULL * 1024 * 1024,
          "in-flight budget must fit the maximum block and be at most 1 GiB");
  auto check_cancelled = [&] {
    if (options.stop.stop_requested())
      throw Cancelled("ingestion cancelled");
  };
  check_cancelled();
  IngestStats stats;
  auto notify = [&](ProgressPhase phase, bool complete = false, std::size_t buffered = 0) {
    if (options.progress)
      invoke_progress(options.progress, {phase, {}, stats.file_bytes, {}, complete, buffered});
    check_cancelled();
  };
  notify(ProgressPhase::ingesting);
  Json blocks = Json::array();
  const auto manifest_base_size = Json{{"version", 1},
                                       {"size", 0},
                                       {"file_hash", std::string(64, '0')},
                                       {"chunking", chunking.json()},
                                       {"blocks", blocks}}
                                      .dump()
                                      .size();
  std::size_t serialized_blocks_size = 0;
  blake3_hasher whole;
  blake3_hasher_init(&whole);
  Bytes buffer(chunking.max);
  std::unique_ptr<EncodePool> pool;
  struct Pending {
    ObjectKey key;
    std::size_t size;
    std::future<Bytes> envelope;
  };
  std::deque<Pending> pending;
  std::set<std::string> scheduled;
  std::size_t in_flight = 0;
  auto publish_first = [&] {
    check_cancelled();
    auto &job = pending.front();
    auto envelope = job.envelope.get();
    check_cancelled();
    atomic_write(object_path(job.key), envelope, false);
    scheduled.erase(job.key.id);
    in_flight -= job.size;
    pending.pop_front();
  };
  auto process_block = [&](std::span<const std::uint8_t> part) -> std::pair<ObjectKey, bool> {
    if (options.workers == 1)
      return put(Kind::chunk, part);
    ObjectKey key{Kind::chunk, object_id(Kind::chunk, part)};
    if (scheduled.contains(key.id) || contains(key))
      return {key, false};
    while (!pending.empty() &&
           (part.size() > options.max_in_flight_bytes - in_flight || pending.size() >= 2 * options.workers))
      publish_first();
    if (!pool)
      pool = std::make_unique<EncodePool>(options.workers, [&](ZSTD_CCtx *context, const ObjectKey &item,
                                                               std::span<const std::uint8_t> data) {
        return encode_object(item, data, impl_->key, context);
      });
    check_cancelled();
    pending.push_back({key, part.size(), pool->submit(key, Bytes(part.begin(), part.end()))});
    scheduled.insert(key.id);
    in_flight += part.size();
    stats.peak_in_flight_bytes = std::max(stats.peak_in_flight_bytes, in_flight);
    stats.peak_in_flight_blocks = std::max(stats.peak_in_flight_blocks, pending.size());
    return {key, true};
  };
  std::size_t length = 0;
  bool eof = false;
  while (true) {
    check_cancelled();
    while (length < buffer.size() && !eof) {
      check_cancelled();
      auto remaining = buffer.size() - length;
      source.read(reinterpret_cast<char *>(buffer.data() + length), static_cast<std::streamsize>(remaining));
      auto got = source.gcount();
      require(got >= 0 && !source.bad(), "input read failed");
      length += static_cast<std::size_t>(got);
      if (source.eof())
        eof = true;
      else
        require(!source.fail(), "input read failed");
    }
    if (length == 0)
      break;
    check_cancelled();
    auto cut = chunk_cut(std::span(buffer.data(), length), chunking);
    require(cut > 0 && cut <= length, "invalid chunker boundary");
    auto part = std::span(buffer.data(), cut);
    auto [key, added] = process_block(part);
    require(cut <= std::numeric_limits<std::uint64_t>::max() - stats.file_bytes, "file size overflow");
    auto entry = Json{{"id", key.id}, {"size", cut}};
    serialized_blocks_size += entry.dump().size() + (stats.blocks ? 1 : 0);
    auto manifest_size =
        manifest_base_size + std::to_string(stats.file_bytes + cut).size() - 1 + serialized_blocks_size;
    require(manifest_size <= max_object_size, "manifest exceeds the v1 metadata size limit");
    blake3_hasher_update(&whole, part.data(), part.size());
    stats.file_bytes += cut;
    if (added)
      stats.new_bytes += cut;
    else
      stats.reused_bytes += cut;
    ++stats.blocks;
    blocks.push_back(std::move(entry));
    std::move(buffer.begin() + static_cast<std::ptrdiff_t>(cut),
              buffer.begin() + static_cast<std::ptrdiff_t>(length), buffer.begin());
    length -= cut;
    notify(ProgressPhase::ingesting, false, pending.size());
  }
  while (!pending.empty())
    publish_first();
  check_cancelled();
  notify(ProgressPhase::publishing);
  Key digest{};
  blake3_hasher_finalize(&whole, digest.data(), digest.size());
  auto manifest = Json{
      {"version", 1},
      {"size", stats.file_bytes},
      {"file_hash", hex(digest)},
      {"chunking", chunking.json()},
      {"blocks",
       blocks}}.dump();
  auto id = put(Kind::manifest, bytes(manifest)).first.id;
  notify(ProgressPhase::ingesting, true);
  return {std::move(id), stats};
}
template <class Reader>
static void stream_manifest(const Json &manifest, std::ostream &output, Reader reader,
                            const std::function<void(std::uint64_t)> &progress = {},
                            bool allow_missing = false) {
  blake3_hasher whole;
  blake3_hasher_init(&whole);
  std::uint64_t processed = 0;
  bool complete = true;
  for (const auto &block : manifest["blocks"]) {
    Bytes data;
    try {
      data = reader(ObjectKey{Kind::chunk, block["id"].get<std::string>()});
    } catch (const MissingObject &) {
      if (!allow_missing)
        throw;
      complete = false;
      continue;
    }
    require(data.size() == block["size"].get<std::size_t>(), "block length mismatch");
    blake3_hasher_update(&whole, data.data(), data.size());
    output.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
    require(output.good(), "output write failed");
    processed += data.size();
    if (progress)
      progress(processed);
  }
  Key digest{};
  blake3_hasher_finalize(&whole, digest.data(), digest.size());
  require(!complete || hex(digest) == manifest["file_hash"].get<std::string>(),
          "reconstructed file hash mismatch");
}
void Store::reconstruct(std::string_view id, std::ostream &output, OperationOptions options) const {
  throw_if_cancelled(options.stop);
  auto session = impl_->packs.read_session();
  auto manifest = metadata(Kind::manifest, id);
  auto total = manifest.at("size").get<std::uint64_t>();
  auto notify = [&](std::uint64_t bytes, bool complete = false) {
    if (options.progress)
      invoke_progress(options.progress, {ProgressPhase::reconstructing, {}, bytes, total, complete});
    throw_if_cancelled(options.stop);
  };
  notify(0);
  stream_manifest(
      manifest, output,
      [&](const ObjectKey &key) {
        throw_if_cancelled(options.stop);
        auto data = get(key);
        throw_if_cancelled(options.stop);
        return data;
      },
      [&](std::uint64_t bytes) { notify(bytes); });
  notify(total, true);
}
void Store::restore_file(std::string_view id, const fs::path &dest, OperationOptions options) const {
  atomic_stream(dest, [&](std::ostream &out) { reconstruct(id, out, options); }, false, options.stop);
}
void Store::reconstruct_range(std::string_view id, std::ostream &output, std::uint64_t offset,
                              std::uint64_t length, OperationOptions options) const {
  throw_if_cancelled(options.stop);
  auto session = impl_->packs.read_session();
  auto manifest = metadata(Kind::manifest, id);
  auto total = manifest.at("size").get<std::uint64_t>();
  require(offset <= total && length <= total - offset, "file range exceeds manifest size");
  if (offset == 0 && length == total) {
    reconstruct(id, output, options);
    return;
  }
  auto notify = [&](std::uint64_t count, bool complete = false) {
    report_progress(options, {ProgressPhase::reconstructing, {}, count, length, complete});
  };
  notify(0);
  const auto end = offset + length;
  std::uint64_t position = 0, written = 0;
  for (const auto &block : manifest["blocks"]) {
    throw_if_cancelled(options.stop);
    if (length == 0 || position >= end)
      break;
    auto size = block["size"].get<std::uint64_t>();
    auto next = position + size; // The canonical manifest already checks the sum.
    if (next > offset) {
      auto data = get({Kind::chunk, block["id"].get<std::string>()});
      throw_if_cancelled(options.stop);
      require(data.size() == size, "block length mismatch");
      auto first = std::max(position, offset) - position;
      auto count = std::min(next, end) - position - first;
      output.write(reinterpret_cast<const char *>(data.data() + first), static_cast<std::streamsize>(count));
      require(output.good(), "output write failed");
      written += count;
      notify(written);
    }
    position = next;
  }
  require(written == length, "file range reconstruction is incomplete");
  notify(written, true);
}
void Store::restore_file_range(std::string_view id, const fs::path &dest, std::uint64_t offset,
                               std::uint64_t length, OperationOptions options) const {
  atomic_stream(
      dest, [&](std::ostream &out) { reconstruct_range(id, out, offset, length, options); }, false,
      options.stop);
}
Refs Store::refs() const {
  return parse_refs(read_bytes(root() / "refs.json", max_object_size), "invalid refs document");
}
std::string_view Store::native_workspace_name() const { return impl_->native_workspace; }
void Store::use_native_identity(NativeIdentity identity, std::optional<SigningKey> signer) {
  require(progress_callback_depth == 0, "cannot select native identity from a progress callback");
  identity.validate();
  if (signer) (void)signer->public_key();
  impl_->native_identity = std::move(identity); impl_->native_signer = std::move(signer);
}
void Store::clear_native_identity() {
  require(progress_callback_depth == 0, "cannot select native identity from a progress callback");
  impl_->native_signer.reset(); impl_->native_identity.reset();
}
std::optional<NativeIdentityContext> Store::take_native_identity() {
  require(progress_callback_depth == 0, "cannot select native identity from a progress callback");
  if (!impl_->native_identity) return {};
  std::optional<NativeIdentityContext> result{NativeIdentityContext{
      std::move(*impl_->native_identity), std::move(impl_->native_signer)}};
  impl_->native_signer.reset(); impl_->native_identity.reset();
  return result;
}
Json Store::native_identity() const {
  if (!impl_->native_identity) return {};
  const auto &identity = *impl_->native_identity;
  Json result{{"name", identity.name}, {"email", identity.email}, {"timezone_offset", identity.timezone_offset},
      {"signing_key", Json{}}, {"public_key", Json{}}};
  if (impl_->native_signer) {
    const auto key = impl_->native_signer->public_key();
    result["signing_key"] = signing_key_id(key); result["public_key"] = hex(key);
  }
  return result;
}
Json Store::native_commit_metadata(Json draft, std::optional<Json> author) const {
  if (!impl_->native_identity) {
    require(draft.value("version", 1) == 1 && (!author || author->is_null()),
        "rewriting authored revisions requires a committer identity");
    return draft;
  }
  const auto committer = native_author_stamp(*impl_->native_identity, draft.at("timestamp"));
  const auto original = author ? canonical_native_author(*author, true) : committer;
  Json result{{"version", 2}, {"parents", draft.at("parents")}, {"message", draft.at("message")},
      {"timestamp", draft.at("timestamp")}, {"author", original}, {"committer", committer},
      {"signature", Json{}}};
  result["files"] = std::move(draft.at("files"));
  if (impl_->native_signer) {
    const auto key = impl_->native_signer->public_key();
    const auto message = native_revision_signing_message(result);
    const auto signature = impl_->native_signer->sign(bytes(message));
    result["signature"] = Json{{"algorithm", "ed25519"}, {"key_id", signing_key_id(key)},
        {"public_key", hex(key)}, {"value", hex(signature)}};
  }
  return result;
}
void Store::use_native_workspace(std::string_view name) {
  require(progress_callback_depth == 0, "cannot select native workspace from a progress callback");
  if (!name.empty()) (void)read_native_workspace_context(*this, name);
  impl_->native_workspace = name;
}
std::string Store::native_workspace_reference(std::string_view local) const {
  return native_context_reference(native_workspace_name(), local);
}
void Store::write_refs(const Refs &refs, std::stop_token stop) const {
  const auto before = this->refs();
  validate_native_workspace_relocation(*this, before, refs);
  validate_native_branch_reservations(*this, before, refs);
  const bool sparse = validate_native_materialization_changes(*this, before, refs);
  // Reference names cannot contain JSON escapes. Reject provably oversized
  // documents early; check the exact encoded size again before publication.
  std::size_t lower_bound = 2;
  for (const auto &[name, key] : refs) {
    const auto size = name.size() + key.id.size() + kind_name(key.kind).size() + 10;
    require(size <= max_object_size - lower_bound, "reference document exceeds 16 MiB");
    lower_bound += size;
  }
  auto value = encode_refs(refs).dump();
  require(value.size() <= max_object_size, "reference document exceeds 16 MiB");
  if (sparse) require_features({"sparse-workspaces-v1"}, {stop});
  for (auto entry = refs.lower_bound("workspaces/");
       entry != refs.end() && entry->first.starts_with("workspaces/"); ++entry) {
    const auto slash = entry->first.find('/', 11);
    if (slash != std::string::npos && entry->first.substr(slash + 1) == "registration") {
      require_features({"linked-workspaces-v1"}, {stop});
      break;
    }
  }
  atomic_write(root() / "refs.json", bytes(value), true, stop);
}
void Store::set_ref(std::string_view name, const ObjectKey &key, std::stop_token stop) const {
  validate_ref(name);
  (void)closure({key}, true, stop);
  auto all = refs();
  all.insert_or_assign(std::string(name), key);
  write_refs(all, stop);
}
bool Store::compare_set_ref(std::string_view name, const std::optional<ObjectKey> &expected,
                            const ObjectKey &key, std::stop_token stop) const {
  throw_if_cancelled(stop);
  validate_ref(name);
  auto all = refs();
  auto found = all.find(std::string(name));
  std::optional<ObjectKey> actual;
  if (found != all.end())
    actual = found->second;
  if (actual != expected)
    return false;
  (void)closure({key}, true, stop);
  all.insert_or_assign(std::string(name), key);
  write_refs(all, stop);
  return true;
}
bool Store::compare_update_refs(const std::vector<RefUpdate> &updates, OperationOptions options) const {
  throw_if_cancelled(options.stop);
  const auto original = read_bytes(root() / "refs.json", max_object_size);
  auto all = parse_refs(original, "invalid refs document");
  std::set<std::string> names;
  std::set<ObjectKey> roots;
  for (const auto &update : updates) {
    throw_if_cancelled(options.stop);
    validate_ref(update.name);
    require(names.insert(update.name).second, "duplicate reference transaction entry");
    if (update.expected)
      (void)key_json(*update.expected);
    if (update.value) {
      (void)key_json(*update.value);
      roots.insert(*update.value);
    }
  }
  for (const auto &update : updates) {
    auto found = all.find(update.name);
    auto actual = found == all.end() ? std::optional<ObjectKey>{} : std::optional{found->second};
    if (actual != update.expected)
      return false;
    if (update.value)
      all[update.name] = *update.value;
    else
      all.erase(update.name);
  }
  if (updates.empty())
    return true;
  (void)closure(std::vector<ObjectKey>(roots.begin(), roots.end()), true, options.stop, options.progress);
  report_progress(options, {ProgressPhase::publishing, "refs"});
  throw_if_cancelled(options.stop);
  // Observers can call reference APIs. Reject stale publication after the last
  // observer instead of overwriting even an unrelated completed update. This
  // is a bounded linear byte comparison, without reparsing a second refs DOM.
  if (read_bytes(root() / "refs.json", max_object_size) != original)
    return false;
  write_refs(all, options.stop);
  return true;
}
void Store::remove_ref(std::string_view name) const {
  validate_ref(name);
  auto all = refs();
  require(all.erase(std::string(name)) == 1, "unknown reference");
  write_refs(all);
}
struct NullBuffer : std::streambuf {
  std::streamsize xsputn(const char *, std::streamsize n) override {
    return n;
  }
  int_type overflow(int_type c) override {
    return traits_type::not_eof(c);
  }
};
template <class Marker, class Reader>
static std::size_t walk_objects(const std::vector<ObjectKey> &roots, bool verify, Marker mark, Reader read,
                                std::stop_token stop = {}) {
  throw_if_cancelled(stop);
  auto pending = roots;
  std::size_t peak = pending.size();
  NullBuffer buffer;
  std::ostream sink(&buffer);
  while (!pending.empty()) {
    throw_if_cancelled(stop);
    auto key = std::move(pending.back());
    pending.pop_back();
    if (!mark(key))
      continue;
    auto data = read(key);
    if (verify && key.kind == Kind::manifest) {
      stream_manifest(parse_json(data), sink, [&](const ObjectKey &block) {
        throw_if_cancelled(stop);
        auto bytes = read(block);
        throw_if_cancelled(stop);
        mark(block);
        return bytes;
      });
    } else {
      auto children = dependencies(key.kind, data);
      pending.insert(pending.end(), children.begin(), children.end());
      peak = std::max(peak, pending.size());
    }
  }
  throw_if_cancelled(stop);
  return peak;
}
template <class Reader>
static std::size_t retain_partial(const std::vector<ObjectKey> &roots, ObjectMarks &live, Reader read,
                                  std::stop_token stop) {
  auto pending = roots;
  std::size_t peak = pending.size();
  NullBuffer buffer;
  std::ostream sink(&buffer);
  while (!pending.empty()) {
    throw_if_cancelled(stop);
    auto key = std::move(pending.back());
    pending.pop_back();
    if (live.contains(key))
      continue;
    Bytes data;
    try {
      data = read(key);
    } catch (const MissingObject &) {
      continue; // A partial pull may not yet have received this dependency.
    }
    live.insert(key); // Missing objects never enter the packed/loose live marks.
    if (key.kind == Kind::manifest) {
      stream_manifest(
          parse_json(data), sink,
          [&](const ObjectKey &block) {
            throw_if_cancelled(stop);
            auto value = read(block);
            live.insert(block);
            return value;
          },
          {}, true);
    } else {
      auto children = dependencies(key.kind, data);
      pending.insert(pending.end(), children.begin(), children.end());
      peak = std::max(peak, pending.size());
    }
  }
  throw_if_cancelled(stop);
  return peak;
}
std::set<ObjectKey> Store::closure(const std::vector<ObjectKey> &roots, bool verify, std::stop_token stop,
                                   ProgressCallback progress) const {
  OperationOptions options{stop, std::move(progress)};
  std::uint64_t scanned = 0;
  auto notify = [&](bool complete = false) {
    report_progress(options, {ProgressPhase::verifying, {}, scanned, {}, complete});
  };
  notify();
  auto session = impl_->packs.read_session();
  std::set<ObjectKey> seen;
  (void)walk_objects(
      roots, verify, [&](const ObjectKey &key) { return seen.insert(key).second; },
      [&](const ObjectKey &key) {
        auto data = get(key);
        scanned += data.size();
        notify();
        return data;
      },
      stop);
  notify(true);
  return seen;
}
template <class Visitor>
static void visit_loose_objects(const fs::path &root, Visitor visitor, std::optional<Kind> only = {},
                                std::stop_token stop = {}) {
  throw_if_cancelled(stop);
  auto directory = root / "objects";
  if (fs::exists(directory))
    require(fs::is_directory(directory) && !fs::is_symlink(directory), "invalid object directory");
  for (auto kind : {Kind::chunk, Kind::manifest, Kind::revision}) {
    if (only && kind != *only)
      continue;
    auto base = directory / kind_name(kind);
    if (!fs::exists(base))
      continue;
    require(fs::is_directory(base) && !fs::is_symlink(base), "invalid object kind directory");
    for (const auto &shard : fs::directory_iterator(base)) {
      throw_if_cancelled(stop);
      require(shard.is_directory() && !shard.is_symlink(), "invalid object shard");
      for (const auto &file : fs::directory_iterator(shard)) {
        throw_if_cancelled(stop);
        auto id = utf8(file.path().filename());
        if (id.starts_with(".tmp")) {
          require(file.is_regular_file() && !file.is_symlink(), "invalid object entry");
          continue;
        }
        const bool canonical = id.size() == 64 && std::all_of(id.begin(), id.end(), [](char c) {
                                 return (c >= 'a' && c <= 'f') || (c >= '0' && c <= '9');
                               });
        // Never skip a canonical object path. Unknown *.tmp entries can be
        // foreign deletion backups; tolerate only an observed disappearance.
        if (!canonical && transient_metadata_disappeared(file.path(), stop))
          continue;
        require(file.is_regular_file() && !file.is_symlink(), "invalid object entry");
        if (!canonical)
          throw Error("invalid loose object filename: " + id);
        require(utf8(shard.path().filename()) == id.substr(0, 2), "wrong object shard");
        visitor(ObjectKey{kind, id});
      }
    }
  }
}
static std::vector<ObjectKey> loose_objects(const fs::path &root) {
  std::vector<ObjectKey> all;
  visit_loose_objects(root, [&](const ObjectKey &key) { all.push_back(key); });
  std::sort(all.begin(), all.end());
  return all;
}
std::vector<ObjectKey> Store::objects() const {
  auto all = loose_objects(root());
  auto packed = impl_->packs.keys();
  all.insert(all.end(), packed.begin(), packed.end());
  std::sort(all.begin(), all.end());
  all.erase(std::unique(all.begin(), all.end()), all.end());
  return all;
}
struct CacheBypass {
  bool &flag;
  bool before;
  explicit CacheBypass(bool &value) : flag(value), before(value) {
    flag = true;
  }
  ~CacheBypass() {
    flag = before;
  }
};
std::size_t Store::fsck(OperationOptions options) const {
  std::uint64_t scanned = 0;
  auto notify = [&](bool complete = false) {
    report_progress(options, {ProgressPhase::verifying, {}, scanned, {}, complete});
  };
  notify();
  CacheBypass audit(impl_->bypass_cache);
  impl_->load_config();
  throw_if_cancelled(options.stop);
  auto session = impl_->packs.read_session();
  ObjectMarks verified(impl_->packs);
  auto check = [&](const ObjectKey &key) {
    throw_if_cancelled(options.stop);
    (void)walk_objects(
        {key}, true, [&](const ObjectKey &object) { return verified.insert(object); },
        [&](const ObjectKey &object) {
          auto data = get(object);
          scanned += data.size();
          notify();
          return data;
        },
        options.stop);
  };
  for (const auto &[name, key] : refs()) {
    (void)name;
    check(key);
  }
  // Audit metadata before standalone chunks: file hashes consume their blocks
  // directly, without first decoding the entire chunk inventory separately.
  for (auto kind : {Kind::revision, Kind::manifest, Kind::chunk}) {
    for (std::size_t segment = 0; segment < impl_->packs.pack_count(); ++segment)
      impl_->packs.visit_kind(segment, kind, [&](const ObjectKey &key, PackStorage::Position position) {
        throw_if_cancelled(options.stop);
        // Ordinary reads prefer loose objects. Check the secondary packed copy
        // even when the logical object was already visited through a reference.
        if (fs::exists(object_path(key))) {
          auto data = decode_envelope(key, impl_->read_pack(impl_->packs, key));
          scanned += data.size();
          notify();
        }
        if (!verified.contains(position))
          check(key);
      });
    visit_loose_objects(root(), check, kind, options.stop);
  }
  notify(true);
  return verified.size();
}
Json Store::gc(bool remove, OperationOptions options) const {
  throw_if_cancelled(options.stop);
  require_no_native_checkout(*this);
  std::vector<ObjectKey> roots, partial;
  transfer_gc_roots(roots, partial, options.stop);
  auto uploads = prepare_upload_gc(options.stop, remove);
  read_gc_roots(roots, options.stop, remove);
  std::uint64_t verified_bytes = 0, planned_bytes = 0, packed_bytes = 0;
  auto notify = [&](ProgressPhase phase, std::uint64_t count, bool complete = false,
                    std::optional<std::uint64_t> total = {}) {
    report_progress(options, {phase, "gc", count, total, complete});
  };
  notify(ProgressPhase::verifying, 0);
  CacheBypass audit(impl_->bypass_cache);
  impl_->load_config();
  for (const auto &[name, key] : refs()) {
    (void)name;
    roots.push_back(key);
  }
  ObjectMarks live(impl_->packs);
  auto peak_pending = [&] {
    auto session = impl_->packs.read_session();
    auto read = [&](const ObjectKey &key) {
      auto data = get(key);
      verified_bytes += data.size();
      notify(ProgressPhase::verifying, verified_bytes);
      return data;
    };
    auto peak =
        walk_objects(roots, true, [&](const ObjectKey &key) { return live.insert(key); }, read, options.stop);
    peak = std::max(peak, retain_partial(partial, live, read, options.stop));
    // Direct inventory marks do not prove a manifest's children were visited.
    // Traverse dependency roots first so shared marks cannot hide those children.
    for (const auto &lease : uploads)
      visit_upload_inventory(
          lease,
          [&](const ObjectKey &key) {
            if (live.contains(key))
              return;
            try {
              (void)read(key);
            } catch (const MissingObject &) {
              return; // Registration can precede both object presence and its dependencies.
            }
            live.insert(key);
          },
          options.stop);
    return peak;
  }();
  notify(ProgressPhase::verifying, verified_bytes, true);
  notify(ProgressPhase::planning, 0);
  std::size_t dead = 0;
  std::uint64_t size = 0;
  bool repack = false;
  auto catalog = impl_->packs.catalog();
  struct Plan {
    std::size_t survivors = 0;
    std::uint64_t bytes = 8;
  };
  std::vector<Plan> plans(catalog.size());
  std::size_t rewritten = 0, dropped = 0;
  std::uint64_t rewrite_bytes = 0;
  {
    auto session = impl_->packs.read_session();
    for (std::size_t i = 0; i < plans.size(); ++i) {
      auto &plan = plans[i];
      impl_->packs.visit(i, [&](const ObjectKey &key, PackStorage::Position position) {
        throw_if_cancelled(options.stop);
        if (live.contains(position)) {
          ++plan.survivors;
          plan.bytes += position.length;
        } else {
          ++dead;
          size += position.length;
          repack = true;
          if (remove)
            impl_->cache.erase(key);
        }
        planned_bytes += position.length;
        notify(ProgressPhase::planning, planned_bytes);
      });
      if (plan.survivors == 0)
        ++dropped;
      else if (plan.survivors < impl_->packs.entry_count(i)) {
        ++rewritten;
        rewrite_bytes += plan.bytes;
      }
    }
    // Validate the complete loose tree before creating or deleting any files.
    // Packed duplicates count once logically, but all dead copies count in bytes.
    visit_loose_objects(
        root(),
        [&](const ObjectKey &key) {
          auto length = fs::file_size(object_path(key));
          if (!live.contains(key)) {
            size += length;
            if (!impl_->packs.contains(key))
              ++dead;
          }
          planned_bytes += length;
          notify(ProgressPhase::planning, planned_bytes);
        },
        {}, options.stop);
  }
  auto obsolete = impl_->packs.cleanup(false, options.stop);
  notify(ProgressPhase::planning, planned_bytes, true);
  if (remove) {
    const auto packing_total = rewrite_bytes - 8 * rewritten;
    if (rewritten)
      notify(ProgressPhase::packing, 0, false, packing_total);
    if (repack) {
      Json updated = Json::array();
      for (std::size_t i = 0; i < plans.size(); ++i) {
        const auto &plan = plans[i];
        if (plan.survivors == impl_->packs.entry_count(i))
          updated.push_back(catalog[i]);
        else if (plan.survivors) {
          auto part = [&] {
            auto session = impl_->packs.read_session();
            return PackStorage::build_stream(
                root(), plan.survivors,
                [&](const PackStorage::KeyVisitor &emit) {
                  impl_->packs.visit(i, [&](const ObjectKey &key, PackStorage::Position position) {
                    throw_if_cancelled(options.stop);
                    if (live.contains(position))
                      emit(key);
                  });
                },
                [&](const ObjectKey &key) {
                  auto envelope = impl_->read_pack(impl_->packs, key);
                  (void)decode_envelope(key, envelope);
                  packed_bytes += envelope.size();
                  notify(ProgressPhase::packing, packed_bytes, false, packing_total);
                  return envelope;
                },
                plan.bytes, impl_->index_cache_budget, options.stop);
          }();
          part.visit(0, [&](const ObjectKey &key, PackStorage::Position) {
            throw_if_cancelled(options.stop);
            auto data = decode_envelope(key, impl_->read_pack(part, key));
            verified_bytes += data.size();
            notify(ProgressPhase::verifying, verified_bytes);
          });
          updated.push_back(part.metadata());
        }
      }
      PackStorage next(root(), impl_->index_cache_budget);
      next.load(updated);
      notify(ProgressPhase::publishing, packed_bytes);
      impl_->publish_pack(std::move(next), options.stop);
    } else {
      notify(ProgressPhase::publishing, 0);
    }
    // Commit point: finish cleanup without callbacks or cancellation. Stopping
    // here could strand dead loose metadata after its packed dependencies left.
    // The bitmap addresses the OLD catalog. After publication use the compact
    // loose marks and the NEW catalog, whose packed keys are all retained.
    auto session = impl_->packs.read_session();
    visit_loose_objects(root(), [&](const ObjectKey &key) {
      if (!live.loose_contains(key) && !impl_->packs.contains(key)) {
        require(fs::remove(object_path(key)), "cannot remove unreachable object");
        impl_->cache.erase(key);
        SYRNCK_IO_CHECKPOINT("gc.loose_removed", object_path(key));
      }
    });
    (void)impl_->packs.cleanup();
  }
  return Json{{"live_objects", live.size()},
              {"unreachable_objects", dead},
              {"reclaimable_bytes", size},
              {"repack_required", repack},
              {"packs_to_rewrite", rewritten},
              {"packs_to_drop", dropped},
              {"pack_bytes_to_write", rewrite_bytes},
              {"obsolete_pack_files", obsolete.first},
              {"obsolete_pack_bytes", obsolete.second},
              {"packed_mark_bytes", live.packed_bytes()},
              {"loose_live_objects", live.loose_size()},
              {"peak_pending_objects", peak_pending},
              {"deleted", remove}};
}
Json Store::pack(PackOptions options, OperationOptions operation) const {
  throw_if_cancelled(operation.stop);
  PackStorage::validate_options(options);
  CacheBypass audit(impl_->bypass_cache);
  (void)fsck(operation); // Includes orphan metadata dependencies and complete file hashes.
  std::uint64_t planned = 0, packed_bytes = 0, verified = 0;
  auto notify = [&](ProgressPhase phase, std::uint64_t count, bool complete = false,
                    std::optional<std::uint64_t> total = {}) {
    report_progress(operation, {phase, "pack", count, total, complete});
  };
  notify(ProgressPhase::planning, 0);
  // Filesystem enumeration is unordered. Only loose-only IDs need sorting;
  // packed IDs already have immutable, ordered indexes that can be merged.
  std::vector<IndexKey> loose;
  {
    auto session = impl_->packs.read_session();
    visit_loose_objects(
        root(),
        [&](const ObjectKey &key) {
          if (!impl_->packs.contains(key))
            loose.push_back(IndexKey::from(key));
        },
        {}, operation.stop);
  }
  std::size_t comparisons = 0;
  std::sort(loose.begin(), loose.end(), [&](const IndexKey &a, const IndexKey &b) {
    if ((++comparisons & 4095) == 0)
      throw_if_cancelled(operation.stop);
    return a < b;
  });
  throw_if_cancelled(operation.stop);
  auto catalog = options.compact ? Json::array() : impl_->packs.catalog();
  const auto old_count = catalog.size();
  std::uint64_t existing_objects = 0;
  for (const auto &metadata : catalog)
    existing_objects += metadata.at("objects").get<std::uint64_t>();
  std::size_t object_count = loose.size();
  if (options.compact)
    for (std::size_t i = 0; i < impl_->packs.pack_count(); ++i)
      object_count += impl_->packs.entry_count(i);
  require(object_count <= PackStorage::max_catalog_entries - existing_objects,
          "pack catalog object limit exceeded");
  std::size_t passes = 0;
  PackStorage::KeySequence source = [&]() -> PackStorage::KeyNext {
    if (++passes == 2) {
      notify(ProgressPhase::planning, planned, true);
      notify(ProgressPhase::packing, 0, false, planned);
    }
    auto packed = options.compact ? impl_->packs.ordered_keys() : PackStorage::KeyNext{};
    auto head = packed ? packed() : std::optional<ObjectKey>{};
    return [&, packed = std::move(packed), head = std::move(head),
            i = std::size_t{0}]() mutable -> std::optional<ObjectKey> {
      throw_if_cancelled(operation.stop);
      if (head && (i == loose.size() || IndexKey::from(*head) < loose[i])) {
        auto result = std::move(head);
        head = packed();
        return result;
      }
      return i < loose.size() ? std::optional(loose[i++].object()) : std::nullopt;
    };
  };
  auto next = [&] {
    auto session = impl_->packs.read_session();
    return PackStorage::build_many_stream(
        root(), object_count, source,
        [&](const ObjectKey &key) {
          auto envelope = read_envelope(key);
          (void)decode_envelope(key, envelope);
          packed_bytes += envelope.size();
          notify(ProgressPhase::packing, packed_bytes, false, planned);
          return envelope;
        },
        [&](const ObjectKey &key) {
          auto length =
              fs::exists(object_path(key)) ? fs::file_size(object_path(key)) : impl_->packs.stored_size(key);
          planned += length;
          notify(ProgressPhase::planning, planned);
          return length;
        },
        options, PackStorage::max_packs - old_count, impl_->index_cache_budget, operation.stop);
  }();
  notify(ProgressPhase::packing, packed_bytes, true, planned);
  notify(ProgressPhase::verifying, 0);
  {
    auto session = next.read_session();
    for (std::size_t i = 0; i < next.pack_count(); ++i)
      next.visit(i, [&](const ObjectKey &key, PackStorage::Position) {
        throw_if_cancelled(operation.stop);
        auto data = decode_envelope(key, impl_->read_pack(next, key));
        verified += data.size();
        notify(ProgressPhase::verifying, verified);
      });
  }
  notify(ProgressPhase::verifying, verified, true);
  const auto created = next.pack_count();
  std::uint64_t written_bytes = 0, written_index_bytes = 0;
  for (const auto &metadata : next.catalog()) {
    written_bytes += metadata.at("bytes").get<std::uint64_t>();
    written_index_bytes += 16 + 45 * metadata.at("objects").get<std::uint64_t>();
    catalog.push_back(metadata);
  }
  if (created || options.compact || impl_->format_version == 1) {
    PackStorage combined(root(), impl_->index_cache_budget);
    combined.load(catalog);
    notify(ProgressPhase::publishing, packed_bytes);
    impl_->publish_pack(std::move(combined), operation.stop);
  } else {
    notify(ProgressPhase::publishing, 0);
  }
  // Commit point: deleting loose copies and obsolete packs must now finish.
  std::size_t removed = 0;
  {
    auto session = impl_->packs.read_session();
    visit_loose_objects(root(), [&](const ObjectKey &key) {
      require(impl_->packs.contains(key), "pack source changed since planning");
      if (fs::remove(object_path(key))) {
        ++removed;
        SYRNCK_IO_CHECKPOINT("pack.loose_removed", object_path(key));
      }
    });
  }
  auto obsolete = impl_->packs.cleanup();
  std::uint64_t pack_bytes = 0, index_bytes = 0, objects = 0;
  for (const auto &metadata : impl_->packs.catalog()) {
    const auto count = metadata.at("objects").get<std::uint64_t>();
    objects += count;
    pack_bytes += metadata.at("bytes").get<std::uint64_t>();
    index_bytes += 16 + 45 * count;
  }
  return Json{{"objects", objects},
              {"pack_bytes", pack_bytes},
              {"index_bytes", index_bytes},
              {"pack_count", impl_->packs.pack_count()},
              {"new_packs", created},
              {"objects_written", object_count},
              {"pack_bytes_written", written_bytes},
              {"index_bytes_written", written_index_bytes},
              {"compacted", options.compact},
              {"loose_objects_removed", removed},
              {"obsolete_pack_files_removed", obsolete.first},
              {"obsolete_pack_bytes_removed", obsolete.second}};
}
} // namespace syrnck
