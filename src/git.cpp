#include "internal.hpp"
#include "git_internal.hpp"
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
  clean(store, input, output, store.settings().ingest_options());
}
void clean(const Store &store, std::istream &input, std::ostream &output, IngestOptions options) {
  throw_if_cancelled(options.stop);
  auto first = prefix(input);
  throw_if_cancelled(options.stop);
  if (auto p = parse_pointer(first)) {
    try {
      require(store.metadata(Kind::manifest, p->manifest)["size"].get<std::uint64_t>() == p->size,
              "pointer size mismatch");
      store.set_ref("git/" + p->manifest, {Kind::manifest, p->manifest}, options.stop);
    } catch (const MissingObject &) {
      // A pointer-only checkout must remain clean without inventing a pin for
      // an incomplete closure. Publication still verifies all referenced data.
    }
    output << first;
    require(output.good(), "filter output write failed");
    return;
  }
  PrefixBuffer buffer(first, input.rdbuf());
  std::istream source(&buffer);
  auto [id, stats] = store.ingest(source, {}, options);
  store.set_ref("git/" + id, {Kind::manifest, id}, options.stop);
  output << Pointer{id, stats.file_bytes}.encode();
  require(output.good(), "filter output write failed");
}
void smudge(const Store &store, std::istream &input, std::ostream &output) {
  smudge(store, input, output, {});
}
void smudge(const Store &store, std::istream &input, std::ostream &output, OperationOptions options) {
  smudge(store, input, output, std::move(options), {});
}
static void pass_through(std::string_view first, std::istream &input, std::ostream &output,
                         OperationOptions options) {
  output << first;
  std::array<char, 8192> buffer{};
  while (input) {
    throw_if_cancelled(options.stop);
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    throw_if_cancelled(options.stop);
    output.write(buffer.data(), input.gcount());
  }
  throw_if_cancelled(options.stop);
  require(!input.bad() && output.good(), "filter pass-through failed");
}
void skip_smudge(std::istream &input, std::ostream &output, OperationOptions options) {
  throw_if_cancelled(options.stop);
  auto first = prefix(input);
  (void)parse_pointer(first);
  throw_if_cancelled(options.stop);
  pass_through(first, input, output, std::move(options));
}
void smudge(const Store &store, std::istream &input, std::ostream &output, OperationOptions options,
            const AssetResolver &resolve) {
  throw_if_cancelled(options.stop);
  auto first = prefix(input);
  throw_if_cancelled(options.stop);
  if (auto p = parse_pointer(first)) {
    auto verify = [&] {
      require(store.metadata(Kind::manifest, p->manifest)["size"].get<std::uint64_t>() == p->size,
              "pointer size mismatch");
      // Verify every block and the complete file hash before output. Reuse an
      // existing pin without rewriting refs on each checkout.
      ObjectKey key{Kind::manifest, p->manifest};
      auto name = "git/" + p->manifest;
      auto refs = store.refs();
      auto found = refs.find(name);
      if (found != refs.end() && found->second == key)
        (void)store.closure({key}, true, options.stop, options.progress);
      else
        store.set_ref(name, key, options.stop);
      // A completed pull may have stopped before finishing the Git pin, or a
      // previous pointer may have declared the wrong size. Finish its staging
      // cleanup even when this retry needs no resolver or network request.
      auto stage = "git-fetch/" + p->manifest;
      auto staged = refs.find(stage);
      if (staged != refs.end() && staged->second == key)
        store.remove_ref(stage);
    };
    try {
      verify();
    } catch (const MissingObject &) {
      if (!resolve)
        throw;
      resolve(*p);
      throw_if_cancelled(options.stop);
      verify();
    }
    store.reconstruct(p->manifest, output, options);
  } else {
    pass_through(first, input, output, std::move(options));
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
std::string detail::shell_quote(const fs::path &p) {
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
  auto command = detail::shell_quote(executable) + " --store " + detail::shell_quote(store) + " git ";
  std::vector<std::pair<std::string, std::string>> config = {
      {"filter.syrnck.clean", command + "clean"},
      {"filter.syrnck.smudge", command + "smudge --checkout-policy"},
      {"filter.syrnck.required", "true"}};
  for (const auto &[name, value] : config) {
    auto current = process({"git", "config", "--get", name}, directory);
    require(current.status != 0 || trim_newline(current.out) == value ||
                (name == "filter.syrnck.smudge" && trim_newline(current.out) == command + "smudge"),
            "existing filter differs; refusing to overwrite it");
  }
  for (const auto &[name, value] : config) {
    auto result = process({"git", "config", "--local", name, value}, directory);
    require(result.status == 0, "git config failed");
  }
}
} // namespace syrnck::git
