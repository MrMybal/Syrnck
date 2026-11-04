#include "internal.hpp"
#include <algorithm>
#include <blake3.h>
#include <limits>
#include <utf8proc.h>

namespace syrnck {
void require(bool condition, std::string_view message) {
  if (!condition)
    throw Error(std::string(message));
}
std::string_view kind_name(Kind kind) {
  switch (kind) {
  case Kind::chunk:
    return "chunk";
  case Kind::manifest:
    return "manifest";
  case Kind::revision:
    return "revision";
  }
  throw Error("unknown object kind");
}
Kind parse_kind(std::string_view s) {
  if (s == "chunk")
    return Kind::chunk;
  if (s == "manifest")
    return Kind::manifest;
  if (s == "revision")
    return Kind::revision;
  throw Error("unknown object kind");
}
void validate_id(std::string_view id) {
  require(id.size() == 64 &&
              std::all_of(id.begin(), id.end(),
                          [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }),
          "object ID must be 64 lowercase hexadecimal characters");
}
std::string ascii_lower(std::string s) {
  for (auto &c : s)
    if (c >= 'A' && c <= 'Z')
      c = static_cast<char>(c - 'A' + 'a');
  return s;
}
std::string portable_fold(std::string_view s) {
  utf8proc_uint8_t *output = nullptr;
  auto size = utf8proc_map(
      reinterpret_cast<const utf8proc_uint8_t *>(s.data()), static_cast<utf8proc_ssize_t>(s.size()), &output,
      static_cast<utf8proc_option_t>(UTF8PROC_STABLE | UTF8PROC_COMPOSE | UTF8PROC_CASEFOLD));
  require(size >= 0, "invalid UTF-8 path");
  std::unique_ptr<utf8proc_uint8_t, decltype(&std::free)> owned(output, &std::free);
  return {reinterpret_cast<const char *>(output), static_cast<std::size_t>(size)};
}
void validate_ref(std::string_view name) {
  require(!name.empty() && name.size() <= 200, "invalid reference name");
  for (char c : name)
    require((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/' ||
                c == '.' || c == '_' || c == '-',
            "invalid reference name");
  for (std::size_t p = 0; p <= name.size();) {
    auto n = name.find('/', p);
    auto part = name.substr(p, n == name.npos ? name.size() - p : n - p);
    require(!part.empty() && part != "." && part != "..", "invalid reference component");
    if (n == name.npos)
      break;
    p = n + 1;
  }
}
void validate_path(std::string_view path) {
  require(!path.empty(), "empty repository path");
  (void)portable_fold(path);
  for (std::size_t offset = 0; offset < path.size();) {
    utf8proc_int32_t codepoint = 0;
    auto n = utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t *>(path.data() + offset),
                              static_cast<utf8proc_ssize_t>(path.size() - offset), &codepoint);
    require(n > 0 && utf8proc_category(codepoint) != UTF8PROC_CATEGORY_CC,
            "control character in repository path");
    offset += static_cast<std::size_t>(n);
  }
  for (unsigned char c : path)
    require(c >= 32 && c != 127 && c != '\\' && c != ':' && c != '<' && c != '>' && c != '"' && c != '|' &&
                c != '?' && c != '*',
            "unsafe repository path");
  for (std::size_t p = 0; p <= path.size();) {
    auto n = path.find('/', p);
    auto part = std::string(path.substr(p, n == path.npos ? path.size() - p : n - p));
    auto lower = ascii_lower(part), stem = lower.substr(0, lower.find('.'));
    require(!part.empty() && part != "." && part != ".." && part.back() != '.' && part.back() != ' ' &&
                lower != ".git" && lower != ".syrnck",
            "unsafe repository component");
    require(stem != "con" && stem != "prn" && stem != "aux" && stem != "nul" &&
                !(stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) &&
                  stem[3] >= '1' && stem[3] <= '9'),
            "reserved Windows filename");
    if (n == path.npos)
      break;
    p = n + 1;
  }
}
std::string hex(std::span<const std::uint8_t> data) {
  constexpr char digits[] = "0123456789abcdef";
  std::string s;
  s.reserve(data.size() * 2);
  for (auto b : data) {
    s.push_back(digits[b >> 4]);
    s.push_back(digits[b & 15]);
  }
  return s;
}
Bytes unhex(std::string_view s) {
  require(s.size() % 2 == 0, "invalid hexadecimal value");
  Bytes out;
  out.reserve(s.size() / 2);
  auto nibble = [](char c) -> unsigned {
    if (c >= '0' && c <= '9')
      return static_cast<unsigned>(c - '0');
    if (c >= 'a' && c <= 'f')
      return static_cast<unsigned>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F')
      return static_cast<unsigned>(c - 'A' + 10);
    throw Error("invalid hexadecimal value");
  };
  for (std::size_t i = 0; i < s.size(); i += 2)
    out.push_back(static_cast<std::uint8_t>((nibble(s[i]) << 4) | nibble(s[i + 1])));
  return out;
}
std::string hash(std::span<const std::uint8_t> data) {
  std::array<std::uint8_t, 32> out{};
  blake3_hasher h;
  blake3_hasher_init(&h);
  blake3_hasher_update(&h, data.data(), data.size());
  blake3_hasher_finalize(&h, out.data(), out.size());
  return hex(out);
}
std::string object_id(Kind kind, std::span<const std::uint8_t> data) {
  blake3_hasher h;
  blake3_hasher_init(&h);
  constexpr char prefix[] = "syrnck-object-v1";
  blake3_hasher_update(&h, prefix, sizeof(prefix));
  auto name = kind_name(kind);
  blake3_hasher_update(&h, name.data(), name.size());
  char zero = 0;
  blake3_hasher_update(&h, &zero, 1);
  blake3_hasher_update(&h, data.data(), data.size());
  Key out{};
  blake3_hasher_finalize(&h, out.data(), out.size());
  return hex(out);
}
namespace {
class JsonDepth : public nlohmann::json_sax<Json> {
  unsigned depth_ = 0;
  bool check() const {
    require(depth_ <= 128, "JSON nesting exceeds 128 levels");
    return true;
  }

public:
  bool null() override {
    return check();
  }
  bool boolean(bool) override {
    return check();
  }
  bool number_integer(number_integer_t) override {
    return check();
  }
  bool number_unsigned(number_unsigned_t) override {
    return check();
  }
  bool number_float(number_float_t, const string_t &) override {
    return check();
  }
  bool string(string_t &) override {
    return check();
  }
  bool binary(binary_t &) override {
    return check();
  }
  bool key(string_t &) override {
    return check();
  }
  bool start_object(std::size_t) override {
    check();
    ++depth_;
    return true;
  }
  bool end_object() override {
    --depth_;
    return true;
  }
  bool start_array(std::size_t size) override {
    return start_object(size);
  }
  bool end_array() override {
    return end_object();
  }
  bool parse_error(std::size_t, const std::string &, const nlohmann::detail::exception &) override {
    return false;
  }
};
template <class Value> Value parse_document(std::span<const std::uint8_t> data) {
  // The DOM callback parser scans the parent for discarded elements after
  // every completed container, even when the callback always returns true.
  // Guard depth in a SAX pass, then use the ordinary DOM parser. Both passes
  // use the same JSON grammar; DOM parsing still throws syntax diagnostics if
  // SAX validation stopped at malformed input. No filtering is performed.
  JsonDepth depth;
  (void)Value::sax_parse(data.begin(), data.end(), &depth);
  return Value::parse(data.begin(), data.end());
}
} // namespace
Json parse_json(std::span<const std::uint8_t> data) {
  return parse_document<Json>(data);
}
Refs parse_refs(std::span<const std::uint8_t> data, std::string_view invalid_document) {
  require(data.size() <= max_object_size, "reference document exceeds 16 MiB");
  // Reference tables do not use canonical field order. An ordered_json object
  // searches a vector for every key, making large tables quadratic to parse.
  // Use the tree-backed DOM here; immutable metadata retains ordered_json.
  auto document = parse_document<nlohmann::json>(data);
  require(document.is_object(), invalid_document);
  Refs refs;
  for (const auto &[name, value] : document.items()) {
    validate_ref(name);
    refs.emplace_hint(refs.end(), name, parse_key(Json(value)));
  }
  return refs;
}
Json encode_refs(const Refs &refs) {
  Json result = Json::object();
  auto &object = result.get_ref<Json::object_t &>();
  object.reserve(refs.size());
  // Refs already provides sorted unique names: avoid ordered_json's linear
  // duplicate-key search on each insertion while preserving identical bytes.
  for (const auto &[name, key] : refs) {
    validate_ref(name);
    object.emplace_back(name, key_json(key));
  }
  return result;
}
Json key_json(const ObjectKey &key) {
  validate_id(key.id);
  return Json{{"kind", kind_name(key.kind)}, {"id", key.id}};
}
static void fields(const Json &j, std::initializer_list<std::string_view> names) {
  require(j.is_object() && j.size() == names.size(), "unknown or missing metadata field");
  for (auto name : names)
    require(j.contains(std::string(name)), "missing metadata field");
}
ObjectKey parse_key(const Json &value) {
  fields(value, {"kind", "id"});
  ObjectKey key{parse_kind(value.at("kind").get<std::string>()), value.at("id").get<std::string>()};
  validate_id(key.id);
  return key;
}
static std::uint64_t number(const Json &value) {
  require(value.is_number_unsigned(), "metadata requires an unsigned integer");
  return value.get<std::uint64_t>();
}
Chunking Chunking::fixed_size(std::uint32_t size) {
  Chunking c;
  c.fixed = true;
  c.min = c.avg = c.max = size;
  c.validate();
  return c;
}
void Chunking::validate() const {
  if (fixed) {
    require(max > 0 && max <= max_object_size, "invalid fixed chunk size");
    return;
  }
  require(min >= 64 && min <= 1048576 && avg >= 256 && avg <= 4194304 && max >= 1024 &&
              max <= max_object_size && min < avg && avg < max && min % 2 == 0 && avg % 2 == 0 &&
              max % 2 == 0,
          "invalid FastCDC sizes");
}
Json Chunking::json() const {
  validate();
  if (fixed)
    return Json{{"algorithm", "fixed"}, {"size", max}};
  return Json{{"algorithm", "fast-cdc2020"}, {"min", min}, {"avg", avg}, {"max", max}};
}
Json IngestStats::json() const {
  return Json{{"file_bytes", file_bytes},
              {"new_bytes", new_bytes},
              {"reused_bytes", reused_bytes},
              {"blocks", blocks},
              {"peak_in_flight_bytes", peak_in_flight_bytes},
              {"peak_in_flight_blocks", peak_in_flight_blocks}};
}
Json StoreIoStats::json() const {
  return Json{{"object_reads", object_reads},
              {"envelope_bytes_read", envelope_bytes_read},
              {"decoded_objects", decoded_objects},
              {"cache_presence_hits", cache_presence_hits},
              {"cache_data_hits", cache_data_hits},
              {"cached_objects", cached_objects},
              {"cached_payloads", cached_payloads},
              {"cached_bytes", cached_bytes},
              {"index_directory_bytes", index_directory_bytes},
              {"index_filter_bytes", index_filter_bytes},
              {"index_cached_bytes", index_cached_bytes},
              {"index_cached_pages", index_cached_pages}};
}
static Json canonical(Kind kind, const Json &j) {
  require(number(j.at("version")) == 1, "unsupported object version");
  if (kind == Kind::manifest) {
    fields(j, {"version", "size", "file_hash", "chunking", "blocks"});
    auto size = number(j.at("size"));
    auto fh = j.at("file_hash").get<std::string>();
    validate_id(fh);
    const auto &cj = j.at("chunking");
    Chunking c;
    auto algo = cj.at("algorithm").get<std::string>();
    auto u32 = [](const Json &v) {
      auto n = number(v);
      require(n <= UINT32_MAX, "chunk size overflow");
      return static_cast<std::uint32_t>(n);
    };
    if (algo == "fixed") {
      fields(cj, {"algorithm", "size"});
      c = Chunking::fixed_size(u32(cj.at("size")));
    } else {
      require(algo == "fast-cdc2020", "unsupported chunker");
      fields(cj, {"algorithm", "min", "avg", "max"});
      c.min = u32(cj.at("min"));
      c.avg = u32(cj.at("avg"));
      c.max = u32(cj.at("max"));
      c.validate();
    }
    require(j.at("blocks").is_array(), "blocks must be an array");
    Json blocks = Json::array();
    std::uint64_t sum = 0;
    for (const auto &b : j.at("blocks")) {
      fields(b, {"id", "size"});
      auto id = b.at("id").get<std::string>();
      validate_id(id);
      auto n = number(b.at("size"));
      require(n > 0 && n <= c.max && n <= UINT64_MAX - sum, "invalid block size");
      sum += n;
      blocks.push_back(Json{{"id", id}, {"size", n}});
    }
    require(sum == size, "manifest size mismatch");
    return Json{
        {"version", 1}, {"size", size}, {"file_hash", fh}, {"chunking", c.json()}, {"blocks", blocks}};
  }
  fields(j, {"version", "parents", "message", "timestamp", "files"});
  require(j.at("parents").is_array() && j.at("files").is_object(), "invalid revision structure");
  Json parents = Json::array();
  for (const auto &p : j.at("parents")) {
    auto id = p.get<std::string>();
    validate_id(id);
    parents.push_back(id);
  }
  std::map<std::string, Json> sorted;
  std::set<std::string> folded;
  for (const auto &[path, entry] : j.at("files").items()) {
    validate_path(path);
    require(folded.insert(portable_fold(path)).second, "case-insensitive path collision");
    fields(entry, {"manifest", "executable"});
    auto id = entry.at("manifest").get<std::string>();
    validate_id(id);
    require(entry.at("executable").is_boolean(), "invalid executable flag");
    sorted.emplace(path, Json{{"manifest", id}, {"executable", entry.at("executable")}});
  }
  for (const auto &path : folded)
    for (auto n = path.find('/'); n != path.npos; n = path.find('/', n + 1))
      require(!folded.contains(path.substr(0, n)), "file/directory path collision");
  Json files = Json::object();
  for (auto &[name, entry] : sorted)
    files[name] = entry;
  return Json{{"version", 1},
              {"parents", parents},
              {"message", j.at("message").get<std::string>()},
              {"timestamp", number(j.at("timestamp"))},
              {"files", files}};
}
void validate_object(Kind kind, std::span<const std::uint8_t> data) {
  require(data.size() <= max_object_size, "object exceeds size limit");
  if (kind == Kind::chunk) {
    require(!data.empty(), "empty chunk");
    return;
  }
  require(canonical(kind, parse_json(data)).dump() == text(data), "noncanonical metadata JSON");
}
std::vector<ObjectKey> dependencies(Kind kind, std::span<const std::uint8_t> data) {
  validate_object(kind, data);
  std::vector<ObjectKey> out;
  if (kind == Kind::chunk)
    return out;
  auto j = parse_json(data);
  if (kind == Kind::manifest) {
    for (auto &b : j["blocks"])
      out.push_back({Kind::chunk, b["id"].get<std::string>()});
  } else {
    for (auto &id : j["parents"])
      out.push_back({Kind::revision, id.get<std::string>()});
    for (auto &f : j["files"])
      out.push_back({Kind::manifest, f["manifest"].get<std::string>()});
  }
  return out;
}
} // namespace syrnck
