#include "internal.hpp"
#include <charconv>
#include <sstream>
#include <syrnck/git.hpp>
namespace syrnck::git {
constexpr std::string_view header = "version syrnck-pointer-v1\n";
std::string Pointer::encode() const {
  validate_id(manifest);
  return std::string(header) + "manifest blake3:" + manifest + "\nsize " + std::to_string(size) + "\n";
}
std::optional<Pointer> parse_pointer(std::string_view s) {
  if (!s.starts_with(header))
    return {};
  require(s.size() <= 1024, "oversized Syrnck pointer");
  auto p = s.substr(header.size());
  require(p.starts_with("manifest blake3:"), "missing pointer manifest");
  p.remove_prefix(16);
  auto n = p.find('\n');
  require(n != p.npos, "invalid pointer manifest line");
  auto id = std::string(p.substr(0, n));
  validate_id(id);
  p.remove_prefix(n + 1);
  require(p.starts_with("size "), "missing pointer size");
  p.remove_prefix(5);
  std::uint64_t size = 0;
  auto [end, ec] = std::from_chars(p.data(), p.data() + p.size(), size);
  require(ec == std::errc{} && end == p.data() + p.size() - 1 && *end == '\n', "invalid pointer size");
  Pointer pointer{id, size};
  require(pointer.encode() == s, "noncanonical Syrnck pointer");
  return pointer;
}
static std::string prefix(std::istream &in) {
  std::array<char, 1025> buffer{};
  in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
  require(!in.bad(), "filter input read failed");
  auto n = in.gcount();
  if (in.eof())
    in.clear();
  else
    require(!in.fail(), "filter input read failed");
  return {buffer.data(), static_cast<std::size_t>(n)};
}
class PrefixBuffer : public std::streambuf {
  std::string prefix_;
  std::streambuf *rest_;
  std::size_t at_ = 0;

public:
  PrefixBuffer(std::string s, std::streambuf *rest) : prefix_(std::move(s)), rest_(rest) {}
  int_type underflow() override {
    return at_ < prefix_.size() ? traits_type::to_int_type(prefix_[at_]) : rest_->sgetc();
  }
  int_type uflow() override {
    return at_ < prefix_.size() ? traits_type::to_int_type(prefix_[at_++]) : rest_->sbumpc();
  }
  std::streamsize xsgetn(char *data, std::streamsize size) override {
    auto count = std::min(static_cast<std::size_t>(size), prefix_.size() - at_);
    std::copy_n(prefix_.data() + at_, count, data);
    at_ += count;
    return static_cast<std::streamsize>(count) +
           rest_->sgetn(data + count, size - static_cast<std::streamsize>(count));
  }
};
void clean(const Store &store, std::istream &input, std::ostream &output) {
  auto first = prefix(input);
  if (auto p = parse_pointer(first)) {
    require(store.metadata(Kind::manifest, p->manifest)["size"].get<std::uint64_t>() == p->size,
            "pointer size mismatch");
    store.set_ref("git/" + p->manifest, {Kind::manifest, p->manifest});
    output << first;
    return;
  }
  PrefixBuffer buffer(first, input.rdbuf());
  std::istream source(&buffer);
  auto [id, stats] = store.ingest(source);
  store.set_ref("git/" + id, {Kind::manifest, id});
  output << Pointer{id, stats.file_bytes}.encode();
  require(output.good(), "filter output write failed");
}
void smudge(const Store &store, std::istream &input, std::ostream &output) {
  auto first = prefix(input);
  if (auto p = parse_pointer(first)) {
    require(store.metadata(Kind::manifest, p->manifest)["size"].get<std::uint64_t>() == p->size,
            "pointer size mismatch");
    store.reconstruct(p->manifest, output);
  } else {
    output << first;
    std::array<char, 8192> buffer{};
    while (input) {
      input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
      output.write(buffer.data(), input.gcount());
    }
    require(!input.bad() && output.good(), "filter pass-through failed");
  }
}
static std::string trim_newline(std::string s) {
  while (!s.empty() && (s.back() == '\r' || s.back() == '\n'))
    s.pop_back();
  return s;
}
fs::path common_store(const fs::path &directory) {
  auto result = process({"git", "rev-parse", "--path-format=absolute", "--git-common-dir"}, directory);
  require(result.status == 0, "directory must already be a Git repository");
  return path_from_utf8(trim_newline(result.out)) / "syrnck";
}
static std::string shell_quote(const fs::path &p) {
  auto value = utf8(p);
  require(value.find_first_of("\r\n\0", 0, 3) == value.npos, "invalid filter path");
  std::string out = "'";
  for (char c : value) {
    if (c == '\'')
      out += "'\"'\"'";
    else
      out += c;
  }
  return out + "'";
}
void install(const fs::path &directory, const fs::path &executable, const fs::path &store) {
  auto command = shell_quote(executable) + " --store " + shell_quote(store) + " git ";
  std::vector<std::pair<std::string, std::string>> config = {{"filter.syrnck.clean", command + "clean"},
                                                             {"filter.syrnck.smudge", command + "smudge"},
                                                             {"filter.syrnck.required", "true"}};
  for (const auto &[name, value] : config) {
    auto current = process({"git", "config", "--get", name}, directory);
    require(current.status != 0 || trim_newline(current.out) == value,
            "existing filter differs; refusing to overwrite it");
  }
  for (const auto &[name, value] : config) {
    auto result = process({"git", "config", "--local", name, value}, directory);
    require(result.status == 0, "git config failed");
  }
}
} // namespace syrnck::git
