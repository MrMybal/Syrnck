#include "internal.hpp"
#include "native_workspace.hpp"
#include <chrono>
#include <fstream>
#include <regex>
#include <sstream>
namespace syrnck {
namespace {
std::uint64_t now() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
          .count());
}
bool inside(const fs::path &path, const fs::path &root) {
  auto p = path.begin();
  for (auto r = root.begin(); r != root.end(); ++r, ++p)
    if (p == path.end() || *p != *r)
      return false;
  return true;
}
struct Rule {
  std::string base;
  std::regex pattern;
  bool negative, dir_only;
};
std::string glob(std::string_view input) {
  std::string out = "^";
  for (std::size_t i = 0; i < input.size(); ++i) {
    char c = input[i];
    if (c == '*') {
      if (i + 1 < input.size() && input[i + 1] == '*') {
        ++i;
        if (i + 1 < input.size() && input[i + 1] == '/') {
          ++i;
          out += "(?:.*/)?";
        } else
          out += ".*";
      } else
        out += "[^/]*";
    } else if (c == '?')
      out += "[^/]";
    else if (c == '[') {
      auto end = input.find(']', i + 1);
      require(end != input.npos, "unclosed ignore character class");
      out += '[';
      auto start = i + 1;
      if (start < end && (input[start] == '!' || input[start] == '^')) {
        out += '^';
        ++start;
      }
      for (auto k = start; k < end; ++k) {
        if (input[k] == '\\')
          out += "\\\\";
        else
          out += input[k];
      }
      out += ']';
      i = end;
    } else {
      if (c == '\\' && i + 1 < input.size())
        c = input[++i];
      if (std::string_view(".()+|^$\\{}").find(c) != std::string_view::npos)
        out += '\\';
      out += c;
    }
  }
  return out + '$';
}
void add_rules(const fs::path &dir, std::string base, std::vector<Rule> &rules, std::size_t *budget = nullptr,
               std::stop_token stop = {}) {
  auto path = dir / ".syrnckignore";
  if (!fs::exists(path))
    return;
  require(!fs::is_symlink(path), "ignore file cannot be a symlink");
  require(fs::is_regular_file(path), "ignore file must be a regular file");
  std::ifstream in(path);
  require(in.is_open(), "cannot read ignore file");
  std::istringstream bounded;
  std::istream *input = &in;
  if (budget) {
    const auto data = read_bytes(path, max_object_size - *budget);
    *budget += data.size();
    bounded.str(text(data));
    input = &bounded;
  }
  std::string line;
  while (std::getline(*input, line)) {
    throw_if_cancelled(stop);
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    while (!line.empty() && line.back() == ' ' && (line.size() < 2 || line[line.size() - 2] != '\\'))
      line.pop_back();
    if (line.empty() || line[0] == '#')
      continue;
    bool neg = line[0] == '!';
    if (neg)
      line.erase(0, 1);
    if (line.empty())
      continue;
    bool anchored = line[0] == '/';
    if (anchored)
      line.erase(0, 1);
    bool dirs = !line.empty() && line.back() == '/';
    if (dirs)
      line.pop_back();
    if (line.empty())
      continue;
    bool basename = !anchored && line.find('/') == line.npos;
    require(!budget || line.size() <= 4096, "native text diff ignore rule exceeds 4096 bytes");
    auto pattern = glob(line);
    if (basename)
      pattern.insert(1, "(?:.*/)?");
    require(!budget || rules.size() < 16384, "native text diff ignore rules exceed 16384 entries");
    rules.push_back({base, std::regex(pattern, std::regex::ECMAScript), neg, dirs});
  }
  require(!input->bad(), "ignore file read failed");
}
bool ignored(const std::string &relative, bool dir, const std::vector<Rule> &rules) {
  bool ignored = false;
  for (const auto &rule : rules) {
    if (rule.dir_only && !dir)
      continue;
    if (!relative.starts_with(rule.base))
      continue;
    auto local = relative.substr(rule.base.size());
    if (std::regex_match(local, rule.pattern))
      ignored = !rule.negative;
  }
  return ignored;
}
} // namespace
void visit_native_files(const fs::path &directory, const fs::path &store_root,
                        const std::function<void(const NativeFile &)> &visit, OperationOptions options) {
  throw_if_cancelled(options.stop);
  auto base = fs::canonical(directory);
  require(inside(store_root, base), "native store must be inside committed directory");
  std::set<std::string> folded;
  std::size_t metadata_bytes = 2;
  std::function<void(const fs::path &, std::vector<Rule>)> scan = [&](const fs::path &dir,
                                                                      std::vector<Rule> rules) {
    throw_if_cancelled(options.stop);
    auto rel = dir.lexically_relative(base);
    std::string prefix = rel == "." ? "" : utf8(rel) + "/";
    if (options.progress)
      options.progress({ProgressPhase::scanning, prefix});
    throw_if_cancelled(options.stop);
    add_rules(dir, prefix, rules);
    for (const auto &entry : fs::directory_iterator(dir)) {
      throw_if_cancelled(options.stop);
      auto path = entry.path();
      auto name = utf8(path.filename());
      if (name == ".git" || name == ".syrnck" || inside(path, store_root))
        continue;
      auto relative = utf8(path.lexically_relative(base));
      auto status = entry.symlink_status();
      bool isdir = fs::is_directory(status);
      if (ignored(relative, isdir, rules))
        continue;
      require(!fs::is_symlink(status), "native symlink support is not implemented");
      if (isdir) {
        scan(path, rules);
        continue;
      }
      require(fs::is_regular_file(status), "special files cannot be committed");
      validate_path(relative);
      require(folded.insert(portable_fold(relative)).second, "case-insensitive path collision");
      // A lower bound on this file's revision entry, with the shortest boolean
      // spelling and a 64-character ID. No valid flat v1 snapshot can exceed
      // the object limit here, even before its header/message are counted.
      const auto minimum = relative.size() + std::string_view(R"({"manifest":"","executable":true})").size() +
                           64 + 3 + (folded.size() > 1 ? 1 : 0);
      require(minimum <= max_object_size - metadata_bytes, "native workspace metadata exceeds 16 MiB");
      metadata_bytes += minimum;
      bool executable = false;
#ifndef _WIN32
      executable =
          (status.permissions() & (fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec)) !=
          fs::perms::none;
#endif
      visit({path, relative, entry.file_size(), entry.last_write_time(), executable});
    }
  };
  scan(base, {});
  for (const auto &path : folded) {
    throw_if_cancelled(options.stop);
    for (auto n = path.find('/'); n != path.npos; n = path.find('/', n + 1))
      require(!folded.contains(path.substr(0, n)), "file/directory path collision");
  }
  throw_if_cancelled(options.stop);
}
std::optional<NativeFile> inspect_native_file(const fs::path &directory, const fs::path &store_root,
                                              std::string_view path, OperationOptions options) {
  throw_if_cancelled(options.stop);
  validate_path(path);
  const auto base = fs::canonical(directory);
  require(base != store_root && inside(store_root, base), "native store must be below checkout workspace");
  auto current = base;
  std::string prefix;
  std::vector<Rule> rules;
  std::size_t budget = 0, depth = 0;
  for (std::size_t start = 0;;) {
    throw_if_cancelled(options.stop);
    require(++depth <= 128, "native text diff directory nesting exceeds 128 levels");
    const auto slash = path.find('/', start);
    const auto component = path.substr(start, slash == path.npos ? path.size() - start : slash - start);
    const bool leaf = slash == path.npos;
    const auto relative = prefix + std::string(component);
    const auto ignore_file = current / ".syrnckignore";
    const auto ignore_status = fs::symlink_status(ignore_file);
    if (fs::exists(ignore_status))
      require(!native_link_like(ignore_file, ignore_status), "ignore file cannot be a symlink");
    add_rules(current, prefix, rules, &budget, options.stop);
    require(!ignored(relative, !leaf, rules), "native text diff path is ignored");
    const auto wanted = portable_fold(component);
    std::optional<fs::directory_entry> found;
    report_progress(options, {ProgressPhase::scanning, relative});
    for (const auto &entry : fs::directory_iterator(current)) {
      throw_if_cancelled(options.stop);
      const auto name = utf8(entry.path().filename());
      require(name.size() + 64 <= max_object_size - budget,
              "native text diff namespace exceeds 16 MiB budget");
      budget += name.size() + 64;
      if (portable_fold(name) != wanted)
        continue;
      require(!found, "case-insensitive path collision");
      found = entry;
    }
    if (!found)
      return {};
    require(utf8(found->path().filename()) == component,
            "native text diff path spelling differs from workspace");
    current = found->path();
    require(!inside(current, store_root), "native text diff cannot inspect protected storage");
    const auto status = found->symlink_status();
    require(!native_link_like(current, status), "native text diff path cannot be a link or reparse point");
    require(leaf ? fs::is_regular_file(status) : fs::is_directory(status),
            "native text diff path is obstructed");
    if (leaf) {
      bool executable = false;
#ifndef _WIN32
      executable =
          (status.permissions() & (fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec)) !=
          fs::perms::none;
#endif
      return NativeFile{current, std::string(path), found->file_size(), found->last_write_time(), executable};
    }
    prefix = relative + '/';
    start = slash + 1;
  }
}
Json ingest_native_file(const Store &store, const NativeFile &file, IngestOptions options,
                        std::string_view changed_message) {
  std::ifstream input(file.path, std::ios::binary);
  require(input.is_open(), "cannot open source file");
  auto file_options = options;
  if (options.progress)
    file_options.progress = [&](const ProgressEvent &progress) {
      auto event = progress;
      event.path = file.relative;
      event.total_bytes = file.size;
      options.progress(event);
    };
  const auto [id, stats] = store.ingest(input, {}, file_options);
  const auto status = fs::symlink_status(file.path);
  bool executable = false;
#ifndef _WIN32
  executable = (status.permissions() &
                (fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec)) != fs::perms::none;
#endif
  require(fs::is_regular_file(status) && stats.file_bytes == file.size &&
              fs::file_size(file.path) == file.size && fs::last_write_time(file.path) == file.modified &&
              executable == file.executable,
          changed_message);
  return Json{{"manifest", id}, {"executable", file.executable}};
}
std::string Store::commit_directory(const fs::path &directory, std::string_view branch,
                                    std::string_view message) const {
  return commit_directory(directory, branch, message, settings().ingest_options());
}
std::string Store::commit_directory(const fs::path &directory, std::string_view branch,
                                    std::string_view message, IngestOptions options) const {
  throw_if_cancelled(options.stop);
  validate_ref(branch);
  require_no_native_checkout(*this);
  require(!refs().contains("staging/" + std::string(branch)),
          "native staging exists; commit with --staged or reset the staging index");
  std::map<std::string, Json> files;
  visit_native_files(directory, root(),
                     [&](const NativeFile &file) {
                       files.emplace(file.relative,
                                     ingest_native_file(*this, file, options,
                                                        "file changed during ingestion; retry commit"));
                     },
                     {options.stop, options.progress});
  Json entries = Json::object();
  auto &object = entries.get_ref<Json::object_t &>();
  object.reserve(files.size());
  for (auto &[name, value] : files)
    object.emplace_back(name, std::move(value));
  Json parents = Json::array();
  auto name = "heads/" + std::string(branch);
  auto all = refs();
  if (all.contains(name)) {
    require(all.at(name).kind == Kind::revision, "branch must point to a revision");
    parents.push_back(all.at(name).id);
  }
  auto value = Json{
      {"version", 1},
      {"parents", parents},
      {"message", message},
      {"timestamp", now()},
      {"files",
       entries}}.dump();
  if (options.progress)
    options.progress({ProgressPhase::publishing, branch});
  throw_if_cancelled(options.stop);
  auto key = put(Kind::revision, bytes(value)).first;
  set_ref(name, key, options.stop);
  return key.id;
}
std::string Store::detach_history(std::string_view branch, std::string_view message) const {
  require_no_native_checkout(*this);
  validate_ref(branch);
  auto name = "heads/" + std::string(branch);
  auto all = refs();
  require(all.contains(name) && all.at(name).kind == Kind::revision, "unknown native branch");
  auto previous = all.at(name);
  auto revision = metadata(Kind::revision, previous.id);
  revision["parents"] = Json::array();
  revision["message"] = message;
  revision["timestamp"] = now();
  set_ref("archive/" + previous.id, previous);
  auto value = revision.dump();
  auto key = put(Kind::revision, bytes(value)).first;
  set_ref(name, key);
  return key.id;
}
void Store::export_revision(std::string_view id, const fs::path &dest, OperationOptions options) const {
  throw_if_cancelled(options.stop);
  auto revision = metadata(Kind::revision, id);
  (void)closure({{Kind::revision, std::string(id)}}, true, options.stop, options.progress);
  std::uint64_t total = 0, completed = 0;
  for (const auto &[name, entry] : revision["files"].items()) {
    (void)name;
    throw_if_cancelled(options.stop);
    auto size = metadata(Kind::manifest, entry["manifest"].get<std::string>())["size"].get<std::uint64_t>();
    require(size <= UINT64_MAX - total, "export byte count overflow");
    total += size;
  }
  auto destination = utf8(dest);
  auto notify = [&](ProgressPhase phase, std::string_view path, std::uint64_t count, bool complete = false) {
    report_progress(options, {phase, path, count, total, complete});
  };
  notify(ProgressPhase::exporting, {}, 0);
  atomic_directory(
      dest,
      [&](const fs::path &base) {
        for (const auto &[name, entry] : revision["files"].items()) {
          throw_if_cancelled(options.stop);
          std::uint64_t file_bytes = 0;
          auto path = base / path_from_utf8(name);
          restore_file(entry["manifest"].get<std::string>(), path,
                       {options.stop, [&](const ProgressEvent &event) {
                          file_bytes = event.bytes;
                          notify(ProgressPhase::exporting, name, completed + file_bytes);
                        }});
          if (entry["executable"].get<bool>())
            make_executable(path);
          completed += file_bytes;
        }
        notify(ProgressPhase::exporting, {}, completed, true);
      },
      [&] { notify(ProgressPhase::publishing, destination, completed); }, options.stop);
}
} // namespace syrnck
