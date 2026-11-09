#pragma once
#include <syrnck/net.hpp>
#include <syrnck/sealed.hpp>
namespace syrnck::net {
// Each endpoint/channel has one protected local freshness checkpoint. Keep
// wire separate from the plaintext Store and from application workspaces.
std::string sealed_remote_checkpoint(const Remote &, std::string_view channel);
Json fetch_sealed_snapshot(const Store &wire, const Store &destination, const Remote &, const SecretKey &,
                           std::string_view channel, const SealedLimits & = {}, OperationOptions = {},
                           TransferOptions = {});
// Default permits only unchanged roots and fast-forward original revisions.
// Deleting/replacing roots or rewriting history requires explicit allow_rewrite.
// This does not relax the server's independent carrier branch policies.
Json push_sealed_snapshot(const Store &source, const Store &wire, const Remote &, const SecretKey &,
                          std::string_view channel, const Refs &roots, bool allow_rewrite = false,
                          const SealedLimits & = {}, OperationOptions = {}, TransferOptions = {});
// Resumes the exact durable carrier/expectation, including uncertain publication.
// An older proven publication never rewinds a newer accepted checkpoint.
Json resume_sealed_transfer(const Store &wire, const Store &destination, const Remote &, const SecretKey &,
                            std::string_view channel, std::string_view session, const SealedLimits & = {},
                            OperationOptions = {}, TransferOptions = {});
} // namespace syrnck::net
