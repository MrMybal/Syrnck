#pragma once
#include "internal.hpp"
namespace syrnck::upload_inventory {
inline constexpr std::uint32_t max_pages = 4096;
inline constexpr std::size_t max_objects = 4096, max_page_bytes = 1024 * 1024;
std::string generation(const UploadLease &);
std::string seed(const UploadLease &);
std::vector<ObjectKey> read_page(const Store &, const UploadLease &, std::uint32_t);
std::string write_page(const Store &, const UploadLease &, std::uint32_t, const std::vector<ObjectKey> &);
void visit(const Store &, const UploadLease &, const std::function<void(const ObjectKey &)> &,
           std::stop_token);
void check_tree(const Store &);
void remove_inactive(const Store &, const std::set<std::string> &live);
} // namespace syrnck::upload_inventory
