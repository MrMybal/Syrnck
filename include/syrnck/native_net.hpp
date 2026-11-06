#pragma once
#include <syrnck/net.hpp>
namespace syrnck::net {
// Fetch updates an independent tracking reference, never the selected branch,
// index or files. All fetched roots remain protected until explicit retirement.
Json fetch_native_branch(const Store &, const Remote &, std::string_view remote_branch,
                         std::string_view remote_name = "origin", OperationOptions = {},
                         TransferOptions = {});
Json push_native_branch(const Store &, const Remote &, std::string_view local_branch,
                        std::string_view remote_branch, OperationOptions = {}, TransferOptions = {});
} // namespace syrnck::net
