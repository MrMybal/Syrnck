#include "git_internal.hpp"
#include <charconv>
#include <sstream>

namespace syrnck::git::detail {
std::vector<TreeEntry> read_tree(const fs::path &directory, const std::string &commit,
                                 OperationOptions operation) {
  validate_oid(commit);
  report_progress(operation, {ProgressPhase::scanning, commit});
  auto tree = local_git(directory, {"ls-tree", "-r", "-t", "-l", "-z", "--full-tree", commit}, {},
                        64 * 1024 * 1024, operation.stop);
  require(tree.status == 0, "cannot enumerate Git tree");
  std::vector<TreeEntry> entries;
  std::size_t at = 0;
  while (at < tree.out.size()) {
    throw_if_cancelled(operation.stop);
    require(entries.size() < 262144, "Git catalog entry limit exceeded");
    auto end = tree.out.find('\0', at), tab = tree.out.find('\t', at);
    require(end != tree.out.npos && tab != tree.out.npos && tab < end, "invalid Git tree response");
    std::istringstream fields(tree.out.substr(at, tab - at));
    TreeEntry entry;
    std::string size, extra;
    require(bool(fields >> entry.mode >> entry.type >> entry.oid >> size) && !(fields >> extra),
            "invalid Git tree response");
    validate_oid(entry.oid);
    if (entry.type == "blob") {
      auto parsed = std::from_chars(size.data(), size.data() + size.size(), entry.size);
      require(!size.empty() && parsed.ec == std::errc{} && parsed.ptr == size.data() + size.size(),
              "invalid Git object size");
      require(entry.mode == "100644" || entry.mode == "100755" || entry.mode == "120000",
              "invalid Git tree response");
    } else {
      require(size == "-" && ((entry.type == "tree" && entry.mode == "040000") ||
                              (entry.type == "commit" && entry.mode == "160000")),
              "invalid Git tree response");
    }
    entry.path = tree.out.substr(tab + 1, end - tab - 1);
    require(!entry.path.empty(), "invalid Git tree response");
    entries.push_back(std::move(entry));
    at = end + 1;
  }
  return entries;
}
void visit_small_blobs(const fs::path &directory, const std::vector<TreeEntry> &entries,
                       const std::string &commit,
                       const std::function<void(const std::string &, std::string_view)> &visit,
                       OperationOptions operation) {
  std::map<std::string, std::size_t> sizes;
  for (const auto &entry : entries) {
    throw_if_cancelled(operation.stop);
    if (entry.regular() && entry.size <= 1024) {
      auto [found, inserted] = sizes.emplace(entry.oid, static_cast<std::size_t>(entry.size));
      require(inserted || found->second == entry.size, "inconsistent Git blob size");
    }
  }
  std::size_t processed = 0;
  for (auto cursor = sizes.begin(); cursor != sizes.end();) {
    std::vector<std::pair<std::string, std::size_t>> batch;
    std::string input;
    while (cursor != sizes.end() && batch.size() < 128) {
      batch.push_back(*cursor);
      input += cursor->first + '\n';
      ++cursor;
    }
    auto output = local_git(directory, {"cat-file", "--batch"}, input, 160 * 1024, operation.stop);
    require(output.status == 0, "cannot read Git pointer blobs");
    std::size_t offset = 0;
    for (const auto &[oid, length] : batch) {
      auto expected = oid + " blob " + std::to_string(length) + '\n';
      require(std::string_view(output.out).substr(offset).starts_with(expected),
              "invalid Git batch response");
      offset += expected.size();
      require(length < output.out.size() - offset && output.out[offset + length] == '\n',
              "invalid Git batch response");
      visit(oid, std::string_view(output.out).substr(offset, length));
      ++processed;
      offset += length + 1;
    }
    require(offset == output.out.size(), "trailing Git batch response");
    report_progress(operation, {ProgressPhase::scanning, commit, processed, sizes.size()});
  }
  report_progress(operation, {ProgressPhase::scanning, commit, processed, sizes.size(), true});
}
} // namespace syrnck::git::detail
