#pragma once
#include "internal.hpp"
namespace syrnck::git::detail {
void validate_oid(std::string_view);
std::string trim_line(std::string);
ProcessResult local_git(const fs::path &, std::vector<std::string>, std::string_view input = {},
                        std::size_t limit = 64 * 1024 * 1024, std::stop_token = {});
std::string resolve_commit(const fs::path &, std::string_view, std::stop_token = {});
std::string shell_quote(const fs::path &);
struct TreeEntry {
  std::string path, oid, mode, type;
  std::uint64_t size = 0;
  bool regular() const {
    return type == "blob" && (mode == "100644" || mode == "100755");
  }
};
std::vector<TreeEntry> read_tree(const fs::path &, const std::string &commit, OperationOptions = {});
void visit_small_blobs(const fs::path &, const std::vector<TreeEntry> &, const std::string &commit,
                       const std::function<void(const std::string &, std::string_view)> &,
                       OperationOptions = {});
void create_branch(const fs::path &, const std::string &reference, const std::string &commit,
                   const std::function<void()> &guard, std::stop_token);
} // namespace syrnck::git::detail
