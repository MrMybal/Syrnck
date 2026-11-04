#pragma once
#include <syrnck/git.hpp>
#include <syrnck/net.hpp>
namespace syrnck::git {
std::string catalog_reference(std::string_view commit);
Json push_assets(const Store &, const Catalog &, const net::Remote &, OperationOptions = {},
                 net::TransferOptions = {});
Json pull_assets(const Store &, const Catalog &, const net::Remote &, OperationOptions = {},
                 net::TransferOptions = {});
// Hydrate exactly the pointer's manifest closure; no Git HEAD or catalog lookup.
Json hydrate_asset(const Store &, const Pointer &, const net::Remote &, OperationOptions = {},
                   net::TransferOptions = {});
} // namespace syrnck::git
