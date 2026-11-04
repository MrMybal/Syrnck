#pragma once
#include <syrnck/git.hpp>
#include <syrnck/net.hpp>
namespace syrnck::git {
std::string catalog_reference(std::string_view commit);
Json push_assets(const Store &, const Catalog &, const net::Remote &, OperationOptions = {},
                 net::TransferOptions = {});
Json pull_assets(const Store &, const Catalog &, const net::Remote &, OperationOptions = {},
                 net::TransferOptions = {});
} // namespace syrnck::git
