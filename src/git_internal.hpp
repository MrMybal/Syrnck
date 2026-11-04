#pragma once
#include "internal.hpp"
namespace syrnck::git::detail {
void validate_oid(std::string_view);
std::string trim_line(std::string);
ProcessResult local_git(const fs::path &, std::vector<std::string>, std::string_view input = {},
                        std::size_t limit = 64 * 1024 * 1024, std::stop_token = {});
std::string resolve_commit(const fs::path &, std::string_view, std::stop_token = {});
} // namespace syrnck::git::detail
