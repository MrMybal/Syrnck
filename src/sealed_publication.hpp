#pragma once
#include <syrnck/sealed_net.hpp>
namespace syrnck::net::sealed_transfer_detail {
PublicationObserver observer(const Store &wire, const Store &plain, const SecretKey &, std::string channel,
                             std::string checkpoint, const SealedLimits &, ProgressCallback);
}
