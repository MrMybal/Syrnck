#include "git_internal.hpp"
#include <syrnck/git_lfs.hpp>
#include <picosha2.h>
#include <algorithm>
#include <charconv>
#include <fstream>
#include <limits>

namespace syrnck::git {
namespace {
constexpr std::string_view lfs_header = "version https://git-lfs.github.com/spec/v1\n";
constexpr std::size_t attributes_limit = 1024 * 1024;
struct Snapshot {
  LfsCatalog catalog;
  std::vector<detail::TreeEntry> tree;
};
Snapshot snapshot(const fs::path &directory, std::string_view revision, OperationOptions operation) {
  Snapshot result{{detail::resolve_commit(directory, revision, operation.stop), {}}, {}};
  result.tree = detail::read_tree(directory, result.catalog.commit, operation);
  std::map<std::string, LfsPointer> pointers;
  detail::visit_small_blobs(
      directory, result.tree, result.catalog.commit,
      [&](const std::string &oid, std::string_view value) {
        throw_if_cancelled(operation.stop);
        if (auto pointer = parse_lfs_pointer(value))
          pointers.emplace(oid, *pointer);
      },
      operation);
  std::set<std::string> paths;
  std::map<std::string, std::uint64_t> sizes;
  for (const auto &entry : result.tree) {
    if (!entry.regular() || entry.size > 1024)
      continue;
    if (auto found_pointer = pointers.find(entry.oid); found_pointer != pointers.end()) {
      const auto &pointer = found_pointer->second;
      validate_path(entry.path);
      require(paths.insert(portable_fold(entry.path)).second, "case-insensitive path collision");
      auto [found, inserted] = sizes.emplace(pointer.oid, pointer.size);
      require(inserted || found->second == pointer.size, "inconsistent Git LFS object size");
      result.catalog.assets.push_back({entry.path, entry.oid, pointer, entry.mode == "100755"});
    }
  }
  std::sort(result.catalog.assets.begin(), result.catalog.assets.end(),
            [](const auto &a, const auto &b) { return a.path < b.path; });
  return result;
}
void cache_root(const fs::path &path) {
  auto status = fs::symlink_status(path);
  require(status.type() == fs::file_type::not_found || (fs::is_directory(status) && !fs::is_symlink(status)),
          "Git LFS object directory must be a regular directory");
}
std::optional<fs::path> cached_object(const fs::path &root, const LfsPointer &pointer) {
  (void)pointer.encode();
  cache_root(root);
  auto at = root;
  for (const auto &part : {pointer.oid.substr(0, 2), pointer.oid.substr(2, 2), pointer.oid}) {
    at /= part;
    auto status = fs::symlink_status(at);
    if (status.type() == fs::file_type::not_found)
      return {};
    require(!fs::is_symlink(status) &&
                (part == pointer.oid ? fs::is_regular_file(status) : fs::is_directory(status)),
            "unsafe Git LFS object cache entry");
  }
  return at;
}
class HashBuffer : public std::streambuf {
  std::streambuf *source_;
  picosha2::hash256_one_by_one hash_;
  std::uint64_t size_ = 0;
  void consume(const char *data, std::size_t size) {
    require(size <= std::numeric_limits<std::uint64_t>::max() - size_, "Git LFS byte counter overflow");
    size_ += size;
    while (size) {
      auto count = std::min<std::size_t>(size, 65536);
      hash_.process(reinterpret_cast<const unsigned char *>(data),
                    reinterpret_cast<const unsigned char *>(data) + count);
      data += count;
      size -= count;
    }
  }

public:
  explicit HashBuffer(std::streambuf *source) : source_(source) {}
  std::uint64_t size() const {
    return size_;
  }
  std::string finish() {
    hash_.finish();
    return picosha2::get_hash_hex_string(hash_);
  }
  int_type underflow() override {
    return source_->sgetc();
  }
  int_type uflow() override {
    auto value = source_->sbumpc();
    if (!traits_type::eq_int_type(value, traits_type::eof())) {
      auto byte = traits_type::to_char_type(value);
      consume(&byte, 1);
    }
    return value;
  }
  std::streamsize xsgetn(char *data, std::streamsize size) override {
    auto count = source_->sgetn(data, size);
    consume(data, static_cast<std::size_t>(count));
    return count;
  }
};
std::string audit(const fs::path &root, const LfsPointer &pointer, OperationOptions operation) {
  auto path = cached_object(root, pointer);
  if (!path)
    return "missing";
  if (fs::file_size(*path) != pointer.size)
    return "size_mismatch";
  std::ifstream input(*path, std::ios::binary);
  require(input.is_open(), "cannot read Git LFS object");
  picosha2::hash256_one_by_one hash;
  std::array<unsigned char, 65536> buffer{};
  std::uint64_t size = 0;
  while (input) {
    throw_if_cancelled(operation.stop);
    input.read(reinterpret_cast<char *>(buffer.data()), buffer.size());
    auto count = static_cast<std::size_t>(input.gcount());
    require(count <= std::numeric_limits<std::uint64_t>::max() - size, "Git LFS byte counter overflow");
    size += count;
    hash.process(buffer.begin(), buffer.begin() + count);
    report_progress(operation, {ProgressPhase::verifying, pointer.oid, size, pointer.size});
  }
  require(!input.bad(), "cannot read Git LFS object");
  hash.finish();
  if (size != pointer.size)
    return "size_mismatch";
  return picosha2::get_hash_hex_string(hash) == pointer.oid ? "verified" : "hash_mismatch";
}
std::pair<std::string, std::string> split_path(std::string_view path) {
  auto slash = path.rfind('/');
  return slash == path.npos
             ? std::pair{std::string{}, std::string(path)}
             : std::pair{std::string(path.substr(0, slash)), std::string(path.substr(slash + 1))};
}
using Tree = std::map<std::string, detail::TreeEntry>;
std::map<std::string, std::string> attributes(const Snapshot &source, const fs::path &directory,
                                              OperationOptions operation) {
  Tree entries;
  for (const auto &entry : source.tree)
    require(entries.emplace(entry.path, entry).second, "invalid Git tree response");
  std::map<std::string, std::string> rules;
  for (const auto &asset : source.catalog.assets) {
    auto [parent, basename] = split_path(asset.path);
    auto name = parent.empty() ? ".gitattributes" : parent + "/.gitattributes";
    require(asset.path != name, "Git LFS migration cannot convert attribute files");
    std::string pattern = "/";
    for (char c : basename) {
      if (c == '[' || c == ']')
        pattern += '\\';
      pattern += c;
    }
    auto rule =
        Json(pattern).dump() + " filter=syrnck -text -diff -merge -ident -working-tree-encoding -eol -crlf\n";
    require(rule.size() <= attributes_limit - rules[name].size(),
            "Git LFS migration attribute limit exceeded");
    rules[name] += rule;
    require(rules.size() <= 4096, "Git LFS migration directory limit exceeded");
  }
  for (auto &[name, value] : rules) {
    throw_if_cancelled(operation.stop);
    std::string original;
    if (auto found = entries.find(name); found != entries.end()) {
      require(found->second.regular() && found->second.size <= attributes_limit,
              "unsupported Git attributes file for LFS migration");
      auto read = detail::local_git(directory, {"cat-file", "blob", found->second.oid}, {}, attributes_limit,
                                    operation.stop);
      require(read.status == 0 && read.out.size() == found->second.size &&
                  read.out.find('\0') == read.out.npos &&
                  !read.out.starts_with("version https://git-lfs.github.com/spec/") &&
                  !read.out.starts_with("version syrnck-pointer-"),
              "unsupported Git attributes file for LFS migration");
      original = std::move(read.out);
      if (!original.empty() && !original.ends_with('\n'))
        original += '\n';
    }
    require(original.size() <= attributes_limit && value.size() <= attributes_limit - original.size(),
            "Git LFS migration attribute limit exceeded");
    value = original + value;
  }
  return rules;
}
std::string written_oid(const ProcessResult &result, std::string_view error) {
  require(result.status == 0, error);
  auto oid = detail::trim_line(result.out);
  detail::validate_oid(oid);
  return oid;
}
std::string write_blob(const fs::path &directory, const std::string &value, std::stop_token stop) {
  return written_oid(
      detail::local_git(directory, {"hash-object", "-w", "--no-filters", "--stdin"}, value, 1024, stop),
      "cannot write migrated Git blob");
}
std::string new_reference(const fs::path &directory, std::string_view name, std::stop_token stop) {
  require(!name.empty() && name.size() <= 200 && name != "HEAD" && name.front() != '-' &&
              name.find_first_of("\r\n\0", 0, 3) == name.npos,
          "invalid Git migration branch");
  auto reference = "refs/heads/" + std::string(name);
  require(detail::local_git(directory, {"check-ref-format", reference}, {}, 1024, stop).status == 0,
          "invalid Git migration branch");
  auto symbolic = detail::local_git(directory, {"symbolic-ref", "--quiet", reference}, {}, 1024, stop);
  require(symbolic.status == 1, "Git migration branch already exists or cannot be inspected");
  auto exists = detail::local_git(directory, {"show-ref", "--verify", "--quiet", reference}, {}, 1024, stop);
  require(exists.status == 1, "Git migration branch already exists or cannot be inspected");
  auto head = detail::local_git(directory, {"symbolic-ref", "--quiet", "HEAD"}, {}, 1024, stop);
  require(head.status == 1 || (head.status == 0 && detail::trim_line(head.out) != reference),
          "Git migration branch is selected by HEAD or a worktree");
  auto worktrees =
      detail::local_git(directory, {"worktree", "list", "--porcelain", "-z"}, {}, 1024 * 1024, stop);
  require(worktrees.status == 0, "cannot inspect Git worktrees for migration");
  std::size_t at = 0;
  while (at < worktrees.out.size()) {
    auto end = worktrees.out.find('\0', at);
    require(end != worktrees.out.npos, "invalid Git worktree response");
    require(std::string_view(worktrees.out).substr(at, end - at) != "branch " + reference,
            "Git migration branch is selected by HEAD or a worktree");
    at = end + 1;
  }
  return reference;
}
} // namespace

std::string LfsPointer::encode() const {
  validate_id(oid);
  return std::string(lfs_header) + "oid sha256:" + oid + "\nsize " + std::to_string(size) + "\n";
}
std::optional<LfsPointer> parse_lfs_pointer(std::string_view value) {
  if (!value.starts_with(lfs_header)) {
    require(!value.starts_with("version https://git-lfs.github.com/spec/"),
            "unsupported Git LFS pointer version");
    require(!value.starts_with("version https://hawser.github.com/spec/"),
            "legacy Git LFS pointer version is not supported");
    return {};
  }
  require(value.size() < 1024, "oversized Git LFS pointer");
  auto rest = value.substr(lfs_header.size());
  require(rest.starts_with("oid sha256:"), "unsupported Git LFS pointer fields");
  rest.remove_prefix(11);
  auto end = rest.find('\n');
  require(end != rest.npos, "invalid Git LFS object line");
  LfsPointer pointer{std::string(rest.substr(0, end)), 0};
  validate_id(pointer.oid);
  rest.remove_prefix(end + 1);
  require(rest.starts_with("size "), "unsupported Git LFS pointer fields");
  rest.remove_prefix(5);
  auto parsed = std::from_chars(rest.data(), rest.data() + rest.size(), pointer.size);
  require(parsed.ec == std::errc{} && parsed.ptr == rest.data() + rest.size() - 1 && *parsed.ptr == '\n',
          "invalid Git LFS pointer size");
  require(pointer.encode() == value, "noncanonical or extended Git LFS pointer");
  return pointer;
}
Json LfsCatalog::json() const {
  Json values = Json::array();
  for (const auto &asset : assets)
    values.push_back(Json{{"path", asset.path},
                          {"blob", asset.blob},
                          {"oid", asset.pointer.oid},
                          {"size", asset.pointer.size},
                          {"executable", asset.executable}});
  return Json{{"version", 1}, {"commit", commit}, {"assets", values}};
}
LfsCatalog lfs_catalog(const fs::path &directory, std::string_view revision, OperationOptions operation) {
  return snapshot(directory, revision, operation).catalog;
}
fs::path lfs_objects(const fs::path &directory, const std::optional<fs::path> &override_path) {
  fs::path root;
  if (override_path)
    root = fs::absolute(*override_path).lexically_normal();
  else {
    auto configured = detail::local_git(directory, {"config", "--get", "lfs.storage"}, {}, 8192);
    require(configured.status == 1, "custom lfs.storage requires an explicit --lfs-objects directory");
    root = common_store(directory).parent_path() / "lfs/objects";
  }
  cache_root(root);
  return root;
}
Json preview_lfs(const LfsCatalog &catalog, const fs::path &objects, OperationOptions operation) {
  detail::validate_oid(catalog.commit);
  auto result = catalog.json();
  std::map<std::string, std::pair<std::uint64_t, std::string>> checked;
  bool ready = !catalog.assets.empty();
  for (std::size_t i = 0; i < catalog.assets.size(); ++i) {
    const auto &asset = catalog.assets[i];
    throw_if_cancelled(operation.stop);
    validate_path(asset.path);
    auto found = checked.find(asset.pointer.oid);
    if (found == checked.end())
      found = checked
                  .emplace(asset.pointer.oid,
                           std::pair{asset.pointer.size, audit(objects, asset.pointer, operation)})
                  .first;
    require(found->second.first == asset.pointer.size, "inconsistent Git LFS object size");
    result["assets"][i]["status"] = found->second.second;
    ready &= found->second.second == "verified";
  }
  result["objects"] = utf8(objects);
  result["unique_objects"] = checked.size();
  result["ready"] = ready;
  return result;
}
Json migrate_lfs(const Store &store, const fs::path &directory, std::string_view revision,
                 const fs::path &objects, std::string_view new_branch, IngestOptions options) {
  OperationOptions operation{options.stop, options.progress};
  auto reference = new_reference(directory, new_branch, options.stop);
  auto source = snapshot(directory, revision, operation);
  require(!source.catalog.assets.empty(), "no Git LFS assets to migrate");
  require(source.catalog.assets.size() <= 65536, "Git LFS migration asset limit exceeded");
  auto rules = attributes(source, directory, operation);
  std::map<std::string, Pointer> imported;
  Json converted = Json::array();
  std::map<std::string, detail::TreeEntry> changes;
  for (const auto &asset : source.catalog.assets) {
    throw_if_cancelled(options.stop);
    auto found = imported.find(asset.pointer.oid);
    if (found == imported.end()) {
      auto path = cached_object(objects, asset.pointer);
      require(path.has_value(), "Git LFS object is missing; fetch it before migration");
      require(fs::file_size(*path) == asset.pointer.size, "Git LFS object size mismatch");
      std::ifstream file(*path, std::ios::binary);
      require(file.is_open(), "cannot read Git LFS object");
      HashBuffer hashed(file.rdbuf());
      std::istream input(&hashed);
      auto file_options = options;
      file_options.progress = [&](const ProgressEvent &event) {
        if (options.progress) {
          auto contextual = event;
          contextual.path = asset.path;
          if (contextual.phase == ProgressPhase::ingesting)
            contextual.total_bytes = asset.pointer.size;
          options.progress(contextual);
        }
      };
      auto [id, stats] = store.ingest(input, {}, file_options);
      require(stats.file_bytes == asset.pointer.size && hashed.size() == asset.pointer.size,
              "Git LFS object size mismatch");
      require(hashed.finish() == asset.pointer.oid, "Git LFS object SHA-256 mismatch");
      found = imported.emplace(asset.pointer.oid, Pointer{id, stats.file_bytes}).first;
    }
    auto pointer = found->second.encode();
    auto blob = write_blob(directory, pointer, options.stop);
    changes[asset.path] = {asset.path, blob, asset.executable ? "100755" : "100644", "blob", pointer.size()};
    converted.push_back(Json{{"path", asset.path},
                             {"oid", asset.pointer.oid},
                             {"manifest", found->second.manifest},
                             {"size", asset.pointer.size}});
  }
  std::map<std::string, Tree> directories;
  directories[""];
  for (const auto &entry : source.tree) {
    auto [parent, basename] = split_path(entry.path);
    require(directories[parent].emplace(basename, entry).second, "invalid Git tree response");
  }
  for (const auto &[name, value] : rules) {
    auto [parent, basename] = split_path(name);
    auto existing = directories[parent].find(basename);
    auto mode = existing == directories[parent].end() ? "100644" : existing->second.mode;
    changes[name] = {name, write_blob(directory, value, options.stop), mode, "blob", value.size()};
  }
  std::set<std::string> dirty;
  for (const auto &[path, entry] : changes) {
    auto [parent, basename] = split_path(path);
    directories[parent][basename] = entry;
    for (;;) {
      dirty.insert(parent);
      if (parent.empty())
        break;
      parent = split_path(parent).first;
    }
  }
  std::string tree;
  for (auto at = dirty.rbegin(); at != dirty.rend(); ++at) {
    throw_if_cancelled(options.stop);
    std::string input;
    for (const auto &[name, entry] : directories.at(*at))
      input += entry.mode + ' ' + entry.type + ' ' + entry.oid + '\t' + name + '\0';
    tree = written_oid(detail::local_git(directory, {"mktree", "-z"}, input, 1024, options.stop),
                       "cannot write migrated Git tree");
    if (!at->empty()) {
      auto [parent, basename] = split_path(*at);
      directories[parent].at(basename).oid = tree;
    }
  }
  auto message = "Migrate Git LFS assets to Syrnck\n\nSource: " + source.catalog.commit + '\n';
  auto commit = written_oid(
      detail::local_git(directory,
                        {"-c", "commit.gpgsign=false", "commit-tree", tree, "-p", source.catalog.commit},
                        message, 1024, options.stop),
      "cannot create Git migration commit");
  auto catalog = git::catalog(directory, commit, operation);
  auto bundle = prepare_bundle(store, catalog, operation);
  Json result{{"source", source.catalog.commit},
              {"commit", commit},
              {"branch", new_branch},
              {"converted_assets", source.catalog.assets.size()},
              {"unique_objects", imported.size()},
              {"bundle", key_json(bundle)},
              {"assets", std::move(converted)},
              {"reconciled", false}};
  report_progress(operation, {ProgressPhase::publishing, reference});
  throw_if_cancelled(options.stop);
  // Check the ref again under Git's prepared transaction lock, including its
  // symbolic type. After commit starts, preserve the publication outcome even
  // if cancellation arrives; retain the prepared Store pin on any failure.
  try {
    detail::create_branch(
        directory, reference, commit, [&] { (void)new_reference(directory, new_branch, options.stop); },
        options.stop);
    return result;
  } catch (const LfsPublicationUncertain &) {
    // A lost acknowledgement can be reconciled only against the exact commit.
    try {
      auto inspect = detail::local_git(
          directory, {"rev-parse", "--verify", "--quiet", "--end-of-options", reference}, {}, 1024);
      if (inspect.status == 0 && detail::trim_line(inspect.out) == commit) {
        result["reconciled"] = true;
        return result;
      }
    } catch (const std::exception &) {
    }
    throw;
  }
}
} // namespace syrnck::git
