#include "internal.hpp"
#include "encode_pool.hpp"
#include "object_cache.hpp"
#include "pack_storage.hpp"
#include "object_marks.hpp"
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
  std::optional<Key> key;
  FileLock lock;
  PackStorage packs;
  ObjectCache cache;
  StoreIoStats io;
  bool bypass_cache = false;
  int format_version = 1;
  std::size_t index_cache_budget;
  Impl(fs::path p, std::optional<Key> k, StoreOptions options)
      : root(std::move(p)), key(k), lock(root / "store.lock"), packs(root, options.pack_index_cache_bytes),
        cache(options), index_cache_budget(options.pack_index_cache_bytes) {}
  void load_config();
  void publish_pack(PackStorage next);
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
void Store::Impl::load_config() {
  packs.invalidate();
  auto config = parse_json(read_bytes(root / "config.json"));
  require(config.is_object(), "invalid store configuration");
  auto version = config.at("version");
  require((version == 1 && config.size() == 2) || ((version == 2 || version == 3) && config.size() == 3),
          "unsupported store configuration");
  auto expected = key ? Json(key_check(*key)) : Json(nullptr);
  require(config.at("key_check") == expected,
          "store encryption key missing, wrong, or supplied for an unencrypted store");
  if (version == 2)
    require(config.at("pack").is_null() || config.at("pack").is_object(), "invalid v2 pack descriptor");
  if (version == 3)
    require(config.at("packs").is_array(), "invalid v3 pack catalog");
  packs.load(version == 3 ? config.at("packs") : (version == 2 ? config.at("pack") : Json(nullptr)));
  format_version = version.get<int>();
}
void Store::Impl::publish_pack(PackStorage next) {
  next.set_index_cache_limit(index_cache_budget);
  const bool multiple = format_version >= 3 || next.pack_count() > 1;
  auto config = Json{{"version", multiple ? 3 : 2},
                     {"key_check", key ? Json(key_check(*key)) : Json(nullptr)},
                     {multiple ? "packs" : "pack", multiple ? next.catalog() : next.metadata()}}
                    .dump();
  try {
    atomic_write(root / "config.json", bytes(config), true);
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
    (void)refs();
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
std::pair<ObjectKey, bool> Store::put(Kind kind, std::span<const std::uint8_t> data) const {
  validate_object(kind, data);
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
  }
  while (!pending.empty())
    publish_first();
  check_cancelled();
  Key digest{};
  blake3_hasher_finalize(&whole, digest.data(), digest.size());
  auto manifest = Json{
      {"version", 1},
      {"size", stats.file_bytes},
      {"file_hash", hex(digest)},
      {"chunking", chunking.json()},
      {"blocks",
       blocks}}.dump();
  return {put(Kind::manifest, bytes(manifest)).first.id, stats};
}
template <class Reader>
static void stream_manifest(const Json &manifest, std::ostream &output, Reader reader) {
  blake3_hasher whole;
  blake3_hasher_init(&whole);
  for (const auto &block : manifest["blocks"]) {
    auto data = reader(ObjectKey{Kind::chunk, block["id"].get<std::string>()});
    require(data.size() == block["size"].get<std::size_t>(), "block length mismatch");
    blake3_hasher_update(&whole, data.data(), data.size());
    output.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
    require(output.good(), "output write failed");
  }
  Key digest{};
  blake3_hasher_finalize(&whole, digest.data(), digest.size());
  require(hex(digest) == manifest["file_hash"].get<std::string>(), "reconstructed file hash mismatch");
}
void Store::reconstruct(std::string_view id, std::ostream &output) const {
  auto session = impl_->packs.read_session();
  stream_manifest(metadata(Kind::manifest, id), output, [&](const ObjectKey &key) { return get(key); });
}
void Store::restore_file(std::string_view id, const fs::path &dest) const {
  atomic_stream(dest, [&](std::ostream &out) { reconstruct(id, out); });
}
Refs Store::refs() const {
  auto j = parse_json(read_bytes(root() / "refs.json"));
  require(j.is_object(), "invalid refs document");
  Refs refs;
  for (auto &[name, value] : j.items()) {
    validate_ref(name);
    refs.emplace(name, parse_key(value));
  }
  return refs;
}
void Store::write_refs(const Refs &refs) const {
  Json j = Json::object();
  for (const auto &[name, key] : refs)
    j[name] = key_json(key);
  auto value = j.dump();
  atomic_write(root() / "refs.json", bytes(value), true);
}
void Store::set_ref(std::string_view name, const ObjectKey &key) const {
  validate_ref(name);
  (void)closure({key}, true);
  auto all = refs();
  all.insert_or_assign(std::string(name), key);
  write_refs(all);
}
bool Store::compare_set_ref(std::string_view name, const std::optional<ObjectKey> &expected,
                            const ObjectKey &key) const {
  validate_ref(name);
  auto all = refs();
  auto found = all.find(std::string(name));
  std::optional<ObjectKey> actual;
  if (found != all.end())
    actual = found->second;
  if (actual != expected)
    return false;
  (void)closure({key}, true);
  all.insert_or_assign(std::string(name), key);
  write_refs(all);
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
static std::size_t walk_objects(const std::vector<ObjectKey> &roots, bool verify, Marker mark, Reader read) {
  auto pending = roots;
  std::size_t peak = pending.size();
  NullBuffer buffer;
  std::ostream sink(&buffer);
  while (!pending.empty()) {
    auto key = std::move(pending.back());
    pending.pop_back();
    if (!mark(key))
      continue;
    auto data = read(key);
    if (verify && key.kind == Kind::manifest) {
      stream_manifest(parse_json(data), sink, [&](const ObjectKey &block) {
        auto bytes = read(block);
        mark(block);
        return bytes;
      });
    } else {
      auto children = dependencies(key.kind, data);
      pending.insert(pending.end(), children.begin(), children.end());
      peak = std::max(peak, pending.size());
    }
  }
  return peak;
}
std::set<ObjectKey> Store::closure(const std::vector<ObjectKey> &roots, bool verify) const {
  auto session = impl_->packs.read_session();
  std::set<ObjectKey> seen;
  (void)walk_objects(
      roots, verify, [&](const ObjectKey &key) { return seen.insert(key).second; },
      [&](const ObjectKey &key) { return get(key); });
  return seen;
}
template <class Visitor>
static void visit_loose_objects(const fs::path &root, Visitor visitor, std::optional<Kind> only = {}) {
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
      require(shard.is_directory() && !shard.is_symlink(), "invalid object shard");
      for (const auto &file : fs::directory_iterator(shard)) {
        require(file.is_regular_file() && !file.is_symlink(), "invalid object entry");
        auto id = utf8(file.path().filename());
        if (id.starts_with(".tmp"))
          continue;
        validate_id(id);
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
std::size_t Store::fsck() const {
  CacheBypass audit(impl_->bypass_cache);
  impl_->load_config();
  auto session = impl_->packs.read_session();
  ObjectMarks verified(impl_->packs);
  auto check = [&](const ObjectKey &key) {
    (void)walk_objects(
        {key}, true, [&](const ObjectKey &object) { return verified.insert(object); },
        [&](const ObjectKey &object) { return get(object); });
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
        // Ordinary reads prefer loose objects. Check the secondary packed copy
        // even when the logical object was already visited through a reference.
        if (fs::exists(object_path(key)))
          (void)decode_envelope(key, impl_->read_pack(impl_->packs, key));
        if (!verified.contains(position))
          check(key);
      });
    visit_loose_objects(root(), check, kind);
  }
  return verified.size();
}
Json Store::gc(bool remove) const {
  CacheBypass audit(impl_->bypass_cache);
  impl_->load_config();
  std::vector<ObjectKey> roots;
  for (const auto &[name, key] : refs()) {
    (void)name;
    roots.push_back(key);
  }
  ObjectMarks live(impl_->packs);
  auto peak_pending = [&] {
    auto session = impl_->packs.read_session();
    return walk_objects(
        roots, true, [&](const ObjectKey &key) { return live.insert(key); },
        [&](const ObjectKey &key) { return get(key); });
  }();
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
    visit_loose_objects(root(), [&](const ObjectKey &key) {
      if (!live.contains(key)) {
        size += fs::file_size(object_path(key));
        if (!impl_->packs.contains(key))
          ++dead;
      }
    });
  }
  auto obsolete = impl_->packs.cleanup(false);
  if (remove) {
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
                    if (live.contains(position))
                      emit(key);
                  });
                },
                [&](const ObjectKey &key) {
                  auto envelope = impl_->read_pack(impl_->packs, key);
                  (void)decode_envelope(key, envelope);
                  return envelope;
                },
                plan.bytes, impl_->index_cache_budget);
          }();
          part.visit(0, [&](const ObjectKey &key, PackStorage::Position) {
            (void)decode_envelope(key, impl_->read_pack(part, key));
          });
          updated.push_back(part.metadata());
        }
      }
      PackStorage next(root(), impl_->index_cache_budget);
      next.load(updated);
      impl_->publish_pack(std::move(next));
    }
    // The bitmap addresses the OLD catalog. After publication use the compact
    // loose marks and the NEW catalog, whose packed keys are all retained.
    auto session = impl_->packs.read_session();
    visit_loose_objects(root(), [&](const ObjectKey &key) {
      if (!live.loose_contains(key) && !impl_->packs.contains(key)) {
        require(fs::remove(object_path(key)), "cannot remove unreachable object");
        impl_->cache.erase(key);
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
Json Store::pack(PackOptions options) const {
  PackStorage::validate_options(options);
  CacheBypass audit(impl_->bypass_cache);
  (void)fsck(); // Includes orphan metadata dependencies and complete file hashes.
  // Filesystem enumeration is unordered. Only loose-only IDs need sorting;
  // packed IDs already have immutable, ordered indexes that can be merged.
  std::vector<IndexKey> loose;
  {
    auto session = impl_->packs.read_session();
    visit_loose_objects(root(), [&](const ObjectKey &key) {
      if (!impl_->packs.contains(key))
        loose.push_back(IndexKey::from(key));
    });
  }
  std::sort(loose.begin(), loose.end());
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
  PackStorage::KeySequence source = [&]() -> PackStorage::KeyNext {
    auto packed = options.compact ? impl_->packs.ordered_keys() : PackStorage::KeyNext{};
    auto head = packed ? packed() : std::optional<ObjectKey>{};
    return [&, packed = std::move(packed), head = std::move(head),
            i = std::size_t{0}]() mutable -> std::optional<ObjectKey> {
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
          return envelope;
        },
        [&](const ObjectKey &key) {
          return fs::exists(object_path(key)) ? fs::file_size(object_path(key))
                                              : impl_->packs.stored_size(key);
        },
        options, PackStorage::max_packs - old_count, impl_->index_cache_budget);
  }();
  {
    auto session = next.read_session();
    for (std::size_t i = 0; i < next.pack_count(); ++i)
      next.visit(i, [&](const ObjectKey &key, PackStorage::Position) {
        (void)decode_envelope(key, impl_->read_pack(next, key));
      });
  }
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
    impl_->publish_pack(std::move(combined));
  }
  std::size_t removed = 0;
  {
    auto session = impl_->packs.read_session();
    visit_loose_objects(root(), [&](const ObjectKey &key) {
      require(impl_->packs.contains(key), "pack source changed since planning");
      if (fs::remove(object_path(key)))
        ++removed;
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
