#include "pack_storage.hpp"
#include <algorithm>
#include <blake3.h>
#include <fstream>
#include <limits>
#include <queue>

namespace syrnck {
namespace {
constexpr std::string_view pack_magic = "SYRPACK1", index_magic = "SYRIDX01";
constexpr std::size_t record_size = 45, index_header_size = 16;
constexpr std::size_t max_envelope_size = max_object_size + 1024 * 1024;
void append_integer(Bytes &data, std::uint64_t value, std::size_t width) {
  for (std::size_t i = 0; i < width; ++i)
    data.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}
bool generated_name(std::string_view name) {
  if (name.size() != 41 || !name.starts_with("pack-") ||
      (name.substr(37) != ".syp" && name.substr(37) != ".syi"))
    return false;
  return std::all_of(name.begin() + 5, name.begin() + 37,
                     [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
} // namespace
PackStorage::PackStorage(fs::path root, std::size_t index_cache_bytes)
    : directory_(std::move(root) / "packs"), index_cache_limit_(index_cache_bytes) {
  require(index_cache_bytes <= 1024ULL * 1024 * 1024, "pack index cache limit is too large");
}
void PackStorage::check_directory() const {
  require(fs::is_directory(directory_) && !fs::is_symlink(directory_), "invalid pack directory");
}
fs::path PackStorage::path(std::size_t segment, std::string_view extension) const {
  require(valid_ && segment < segments_.size(), "pack index unavailable");
  return directory_ / path_from_utf8("pack-" + segments_[segment].metadata.at("name").get<std::string>() +
                                     std::string(extension));
}
void PackStorage::check_file(const fs::path &file, std::uint64_t size) const {
  check_directory();
  require(fs::is_regular_file(file) && !fs::is_symlink(file), "missing or invalid pack file");
  require(fs::file_size(file) == size, "pack file size mismatch");
}
void PackStorage::load(const Json &value) {
  valid_ = false; // A failed reload must not leave an old index usable.
  close_streams();
  clear_index_cache();
  std::vector<Segment> segments;
  auto catalog = value.is_array() ? value : Json::array();
  if (value.is_object())
    catalog.push_back(value);
  require(value.is_null() || value.is_object() || value.is_array(), "invalid pack catalog");
  // value may alias metadata(); copy it before releasing the previous snapshot.
  segments_.clear();
  metadata_ = nullptr;
  require(catalog.size() <= max_packs, "too many active packs; use pack --compact");
  std::set<std::string> names;
  std::uint64_t total = 0;
  // Preflight all advertised counts before allocating or reading large indexes.
  for (const auto &metadata : catalog) {
    require(metadata.is_object() && metadata.size() == 4, "invalid pack metadata");
    auto name = metadata.at("name").get<std::string>();
    require(generated_name("pack-" + name + ".syp"), "invalid pack name");
    require(names.insert(name).second, "duplicate pack name");
    require(metadata.at("objects").is_number_unsigned() && metadata.at("bytes").is_number_unsigned(),
            "invalid pack sizes");
    auto count = metadata.at("objects").get<std::uint64_t>();
    auto size = metadata.at("bytes").get<std::uint64_t>();
    require(count > 0 && count <= max_entries && size <= INT64_MAX, "pack limits exceeded");
    require(count <= max_catalog_entries - total, "pack catalog object limit exceeded");
    total += count;
  }
  for (const auto &metadata : catalog) {
    auto name = metadata.at("name").get<std::string>();
    auto count = metadata.at("objects").get<std::uint64_t>();
    auto size = metadata.at("bytes").get<std::uint64_t>();
    auto index_path = directory_ / path_from_utf8("pack-" + name + ".syi");
    check_file(index_path, index_header_size + record_size * count);
    auto index = PackIndex::scan(index_path, static_cast<std::size_t>(count), size,
                                 metadata.at("index_hash").get<std::string>());
    auto pack_path = directory_ / path_from_utf8("pack-" + name + ".syp");
    check_file(pack_path, size);
    std::ifstream in(pack_path, std::ios::binary);
    std::array<char, 8> header{};
    in.read(header.data(), header.size());
    require(in.good() && std::equal(pack_magic.begin(), pack_magic.end(), header.begin()),
            "invalid pack header");
    segments.push_back(Segment{metadata, std::move(index), {}, 0, {}, 0});
  }
  metadata_ = catalog.empty() ? Json(nullptr) : (catalog.size() == 1 ? catalog[0] : catalog);
  segments_ = std::move(segments);
  validate_unique();
  valid_ = true;
}
const Json &PackStorage::metadata() const {
  require(valid_, "pack index unavailable; reopen the store");
  return metadata_;
}
Json PackStorage::catalog() const {
  (void)metadata();
  Json result = Json::array();
  for (const auto &segment : segments_)
    result.push_back(segment.metadata);
  return result;
}
std::size_t PackStorage::pack_count() const {
  (void)metadata();
  return segments_.size();
}
std::size_t PackStorage::open_streams() const {
  return static_cast<std::size_t>(std::count_if(segments_.begin(), segments_.end(),
                                                [](const Segment &segment) { return !!segment.stream; }));
}
std::size_t PackStorage::open_index_streams() const {
  return static_cast<std::size_t>(std::count_if(
      segments_.begin(), segments_.end(), [](const Segment &segment) { return !!segment.index_stream; }));
}
void PackStorage::close_streams() const {
  for (const auto &segment : segments_) {
    segment.stream.reset();
    segment.index_stream.reset();
  }
  read_clock_ = 0;
}
std::istream *PackStorage::index_stream(std::size_t segment) const {
  if (!read_depth_)
    return nullptr;
  const auto &current = segments_[segment];
  if (!current.index_stream) {
    if (open_index_streams() >= max_open_streams) {
      const Segment *oldest = nullptr;
      for (const auto &candidate : segments_)
        if (candidate.index_stream && (!oldest || candidate.index_touched < oldest->index_touched))
          oldest = &candidate;
      oldest->index_stream.reset();
    }
    current.index_stream = current.index.open();
  }
  if (read_clock_ == UINT64_MAX) {
    for (const auto &candidate : segments_) {
      candidate.touched = 0;
      candidate.index_touched = 0;
    }
    read_clock_ = 0;
  }
  current.index_touched = ++read_clock_;
  return current.index_stream.get();
}
void PackStorage::clear_index_cache() const {
  index_cache_.clear();
  index_lru_.clear();
  Bytes{}.swap(index_scratch_);
  index_cached_bytes_ = 0;
  last_lookup_.reset();
}
void PackStorage::set_index_cache_limit(std::size_t limit) {
  require(limit <= 1024ULL * 1024 * 1024, "pack index cache limit is too large");
  clear_index_cache();
  index_cache_limit_ = limit;
}
PackStorage::IndexStats PackStorage::index_stats() const {
  IndexStats result{index_page_reads_, index_cache_hits_};
  result.cache_bytes = index_cached_bytes_;
  result.cached_pages = index_cache_.size();
  for (const auto &segment : segments_) {
    result.directory_bytes += segment.index.directory_bytes();
    result.filter_bytes += segment.index.filter_bytes();
  }
  return result;
}
const Bytes &PackStorage::index_page(std::size_t segment, std::size_t page) const {
  PageKey key{segment, page};
  if (auto found = index_cache_.find(key); found != index_cache_.end()) {
    ++index_cache_hits_;
    index_lru_.splice(index_lru_.begin(), index_lru_, found->second.lru);
    return found->second.data;
  }
  auto data = segments_[segment].index.read_page(page, index_stream(segment));
  ++index_page_reads_;
  if (data.size() > index_cache_limit_) {
    index_scratch_ = std::move(data);
    return index_scratch_;
  }
  while (data.size() > index_cache_limit_ - index_cached_bytes_) {
    auto oldest = index_cache_.find(index_lru_.back());
    index_cached_bytes_ -= oldest->second.data.size();
    index_cache_.erase(oldest);
    index_lru_.pop_back();
  }
  index_lru_.push_front(key);
  try {
    auto found = index_cache_.emplace(key, CachedPage{std::move(data), index_lru_.begin()}).first;
    index_cached_bytes_ += found->second.data.size();
    return found->second.data;
  } catch (...) {
    index_lru_.pop_front();
    throw;
  }
}
std::optional<PackStorage::Entry> PackStorage::find(const ObjectKey &key) const {
  (void)metadata();
  auto raw = IndexKey::from(key);
  if (index_cache_limit_ && last_lookup_ && last_lookup_->first == raw)
    return last_lookup_->second;
  for (std::size_t segment = 0; segment < segments_.size(); ++segment) {
    auto candidate = segments_[segment].index.candidate(raw);
    if (!candidate)
      continue;
    const auto &data = index_page(segment, *candidate);
    std::size_t first = 0, last = data.size() / record_size;
    while (first < last) {
      auto middle = first + (last - first) / 2;
      auto item = PackIndex::record(data, middle);
      if (item.key < raw)
        first = middle + 1;
      else
        last = middle;
    }
    if (first < data.size() / record_size) {
      auto item = PackIndex::record(data, first);
      if (item.key == raw) {
        Entry result{item.offset, item.length, segment, *candidate * PackIndex::records_per_page + first};
        if (index_cache_limit_)
          last_lookup_ = std::pair(raw, result);
        return result;
      }
    }
  }
  return {};
}
void PackStorage::validate_unique() const {
  if (segments_.size() < 2)
    return;
  std::vector<std::size_t> order;
  for (std::size_t i = 0; i < segments_.size(); ++i)
    order.push_back(i);
  std::sort(order.begin(), order.end(),
            [&](auto a, auto b) { return segments_[a].index.first() < segments_[b].index.first(); });
  bool disjoint = true;
  for (std::size_t i = 1; i < order.size(); ++i)
    if (segments_[order[i - 1]].index.last() >= segments_[order[i]].index.first())
      disjoint = false;
  if (disjoint)
    return;
  struct Cursor {
    std::size_t page = 0, ordinal = 0;
    Bytes data;
  };
  struct Head {
    IndexKey key;
    std::size_t segment;
  };
  auto greater = [](const Head &a, const Head &b) { return a.key > b.key; };
  std::priority_queue<Head, std::vector<Head>, decltype(greater)> heap(greater);
  std::vector<Cursor> cursors(segments_.size());
  for (std::size_t i = 0; i < segments_.size(); ++i) {
    cursors[i].data = segments_[i].index.read_page(0);
    heap.push(Head{PackIndex::record(cursors[i].data, 0).key, i});
  }
  std::optional<IndexKey> previous;
  while (!heap.empty()) {
    auto head = heap.top();
    heap.pop();
    require(!previous || *previous != head.key, "object belongs to multiple active packs");
    previous = head.key;
    auto &cursor = cursors[head.segment];
    if (++cursor.ordinal == cursor.data.size() / record_size) {
      if (++cursor.page == segments_[head.segment].index.page_count())
        continue;
      cursor.data = segments_[head.segment].index.read_page(cursor.page);
      cursor.ordinal = 0;
    }
    heap.push(Head{PackIndex::record(cursor.data, cursor.ordinal).key, head.segment});
  }
}
std::vector<std::vector<ObjectKey>> PackStorage::groups() const {
  auto session = read_session();
  std::vector<std::vector<ObjectKey>> result(segments_.size());
  for (std::size_t segment = 0; segment < segments_.size(); ++segment) {
    auto &keys = result[segment];
    keys.reserve(segments_[segment].index.count());
    for (std::size_t page = 0; page < segments_[segment].index.page_count(); ++page) {
      const auto &data = index_page(segment, page);
      for (std::size_t i = 0; i < data.size() / record_size; ++i)
        keys.push_back(PackIndex::record(data, i).key.object());
    }
  }
  return result;
}
std::size_t PackStorage::entry_count(std::size_t segment) const {
  (void)metadata();
  require(segment < segments_.size(), "pack index unavailable");
  return segments_[segment].index.count();
}
void PackStorage::visit(std::size_t segment,
                        const std::function<void(const ObjectKey &, Position)> &visitor) const {
  auto session = read_session();
  (void)entry_count(segment);
  for (std::size_t page = 0; page < segments_[segment].index.page_count(); ++page) {
    // Own this page: a nested lookup can evict any cached page or replace scratch.
    auto data = segments_[segment].index.read_page(page, index_stream(segment));
    ++index_page_reads_;
    for (std::size_t i = 0; i < data.size() / record_size; ++i) {
      auto item = PackIndex::record(data, i);
      visitor(item.key.object(), Position{segment, page * PackIndex::records_per_page + i, item.length});
    }
  }
}
std::optional<PackStorage::Position> PackStorage::locate(const ObjectKey &key) const {
  auto found = find(key);
  if (!found)
    return {};
  return Position{found->segment, found->ordinal, found->length};
}
bool PackStorage::contains(const ObjectKey &key) const { return find(key).has_value(); }
std::vector<ObjectKey> PackStorage::keys() const {
  auto session = read_session();
  std::vector<ObjectKey> result;
  std::size_t count = 0;
  for (const auto &segment : segments_)
    count += segment.index.count();
  result.reserve(count);
  for (std::size_t segment = 0; segment < segments_.size(); ++segment)
    for (std::size_t page = 0; page < segments_[segment].index.page_count(); ++page) {
      const auto &data = index_page(segment, page);
      for (std::size_t i = 0; i < data.size() / record_size; ++i)
        result.push_back(PackIndex::record(data, i).key.object());
    }
  std::sort(result.begin(), result.end());
  return result;
}
std::unique_ptr<std::ifstream> PackStorage::open(std::size_t segment) const {
  auto file = path(segment, ".syp");
  check_file(file, segments_[segment].metadata.at("bytes").get<std::uint64_t>());
  auto in = std::make_unique<std::ifstream>(file, std::ios::binary);
  std::array<char, 8> header{};
  in->read(header.data(), header.size());
  require(in->good() && std::equal(pack_magic.begin(), pack_magic.end(), header.begin()),
          "invalid pack header");
  return in;
}
Bytes PackStorage::read(const ObjectKey &key) const {
  (void)metadata();
  auto found = find(key);
  if (!found)
    throw MissingObject("packed object not found: " + key.id);
  const auto &segment = segments_[found->segment];
  std::unique_ptr<std::ifstream> single;
  if (read_depth_ == 0)
    single = open(found->segment);
  else {
    if (!segment.stream) {
      if (open_streams() >= max_open_streams) {
        const Segment *oldest = nullptr;
        for (const auto &candidate : segments_)
          if (candidate.stream && (!oldest || candidate.touched < oldest->touched))
            oldest = &candidate;
        oldest->stream.reset();
      }
      segment.stream = open(found->segment);
    }
    if (read_clock_ == UINT64_MAX) {
      for (const auto &candidate : segments_) {
        candidate.touched = 0;
        candidate.index_touched = 0;
      }
      read_clock_ = 0;
    }
    segment.touched = ++read_clock_;
  }
  auto &in = single ? *single : *segment.stream;
  in.clear();
  in.seekg(static_cast<std::streamoff>(found->offset));
  Bytes result(found->length);
  in.read(reinterpret_cast<char *>(result.data()), static_cast<std::streamsize>(result.size()));
  require(in.good() && static_cast<std::size_t>(in.gcount()) == result.size(), "pack read failed");
  return result;
}
std::uint64_t PackStorage::stored_size(const ObjectKey &key) const {
  auto item = find(key);
  if (!item)
    throw MissingObject("packed object not found: " + key.id);
  return item->length;
}
PackStorage PackStorage::build(const fs::path &root, const std::vector<ObjectKey> &keys,
                               const std::function<Bytes(const ObjectKey &)> &reader, std::uint64_t max_bytes,
                               std::size_t index_cache_bytes) {
  return build_stream(
      root, keys.size(),
      [&](const KeyVisitor &emit) {
        for (const auto &key : keys)
          emit(key);
      },
      reader, max_bytes, index_cache_bytes);
}
PackStorage PackStorage::build_stream(const fs::path &root, std::size_t count, const KeySource &source,
                                      const std::function<Bytes(const ObjectKey &)> &reader,
                                      std::uint64_t max_bytes, std::size_t index_cache_bytes) {
  require(count <= max_entries, "pack object count exceeds the current 1M index limit");
  PackStorage result(root, index_cache_bytes);
  if (count == 0) {
    source([](const ObjectKey &) { throw Error("pack source count mismatch"); });
    return result;
  }
  fs::create_directories(result.directory_);
  result.check_directory();
  auto name = hex(random_bytes(16));
  auto pack_path = result.directory_ / path_from_utf8("pack-" + name + ".syp");
  auto index_path = result.directory_ / path_from_utf8("pack-" + name + ".syi");
  blake3_hasher index_hash;
  blake3_hasher_init(&index_hash);
  std::uint64_t offset = pack_magic.size();
  std::optional<ObjectKey> previous;
  atomic_stream(index_path, [&](std::ostream &index_out) {
    auto write_index = [&](std::span<const std::uint8_t> data) {
      index_out.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
      require(index_out.good(), "pack index write failed");
      blake3_hasher_update(&index_hash, data.data(), data.size());
    };
    Bytes header(index_magic.begin(), index_magic.end());
    append_integer(header, count, 8);
    write_index(header);
    Bytes page;
    page.reserve(PackIndex::page_bytes);
    atomic_stream(pack_path, [&](std::ostream &out) {
      out.write(pack_magic.data(), pack_magic.size());
      std::size_t emitted = 0;
      source([&](const ObjectKey &key) {
        require(emitted < count, "pack source count mismatch");
        auto raw = IndexKey::from(key);
        require(!previous || *previous < key, "pack input must be strictly sorted");
        auto envelope = reader(key); // Caller validates exactly these envelope bytes.
        require(envelope.size() >= 5 && envelope.size() <= max_envelope_size && offset <= max_bytes &&
                    envelope.size() <= max_bytes - offset && max_bytes <= INT64_MAX,
                "pack size limit exceeded");
        page.insert(page.end(), raw.value.begin(), raw.value.end());
        append_integer(page, offset, 8);
        append_integer(page, envelope.size(), 4);
        if (page.size() == PackIndex::page_bytes) {
          write_index(page);
          page.clear();
        }
        out.write(reinterpret_cast<const char *>(envelope.data()),
                  static_cast<std::streamsize>(envelope.size()));
        require(out.good(), "pack write failed");
        offset += envelope.size();
        previous = key;
        ++emitted;
      });
      require(emitted == count, "pack source count mismatch");
    });
    write_index(page);
  });
  Key sum{};
  blake3_hasher_finalize(&index_hash, sum.data(), sum.size());
  result.load(Json{{"name", name}, {"objects", count}, {"bytes", offset}, {"index_hash", hex(sum)}});
  return result;
}
void PackStorage::validate_options(const PackOptions &options) {
  require(options.max_objects_per_pack > 0 && options.max_objects_per_pack <= max_entries,
          "pack object limit must be in 1..1048576");
  require(options.max_pack_bytes >= max_envelope_size + pack_magic.size() &&
              options.max_pack_bytes <= 4ULL * 1024 * 1024 * 1024,
          "pack byte limit must fit a maximum envelope and be at most 4 GiB");
}
PackStorage PackStorage::build_many(const fs::path &root, const std::vector<ObjectKey> &keys,
                                    const std::function<Bytes(const ObjectKey &)> &reader,
                                    const std::function<std::uint64_t(const ObjectKey &)> &sizer,
                                    const PackOptions &options, std::size_t available_packs,
                                    std::size_t index_cache_bytes) {
  validate_options(options);
  require(keys.size() <= max_catalog_entries, "pack catalog object limit exceeded");
  std::vector<std::pair<std::size_t, std::size_t>> plan;
  std::size_t start = 0;
  std::uint64_t size = pack_magic.size();
  for (std::size_t i = 0; i < keys.size(); ++i) {
    require(i == 0 || keys[i - 1] < keys[i], "pack input must be strictly sorted");
    auto length = sizer(keys[i]);
    require(length >= 5 && length <= max_envelope_size, "invalid envelope length");
    if (i > start && (i - start >= options.max_objects_per_pack || length > options.max_pack_bytes - size)) {
      plan.emplace_back(start, i);
      start = i;
      size = pack_magic.size();
    }
    size += length;
  }
  if (start < keys.size())
    plan.emplace_back(start, keys.size());
  require(plan.size() <= std::min(max_packs, available_packs),
          "too many active packs; use pack --compact or increase pack limits");
  Json catalog = Json::array();
  for (const auto &[begin, end] : plan) {
    std::vector<ObjectKey> part(keys.begin() + static_cast<std::ptrdiff_t>(begin),
                                keys.begin() + static_cast<std::ptrdiff_t>(end));
    auto pack = build(root, part, reader, options.max_pack_bytes, index_cache_bytes);
    catalog.push_back(pack.metadata());
  }
  PackStorage result(root, index_cache_bytes);
  result.load(catalog);
  return result;
}
std::pair<std::size_t, std::uint64_t> PackStorage::cleanup(bool remove) const {
  (void)metadata();
  if (!fs::exists(directory_))
    return {};
  check_directory();
  std::set<fs::path> active;
  for (std::size_t i = 0; i < segments_.size(); ++i) {
    active.insert(path(i, ".syp"));
    active.insert(path(i, ".syi"));
  }
  std::vector<fs::path> obsolete;
  std::uint64_t size = 0;
  for (const auto &file : fs::directory_iterator(directory_)) {
    if (!generated_name(utf8(file.path().filename())))
      continue;
    require(file.is_regular_file() && !file.is_symlink(), "invalid pack entry");
    if (active.contains(file.path()))
      continue;
    obsolete.push_back(file.path());
    size += file.file_size();
  }
  if (remove)
    for (const auto &file : obsolete)
      require(fs::remove(file), "cannot remove obsolete pack file");
  return {obsolete.size(), size};
}
} // namespace syrnck
