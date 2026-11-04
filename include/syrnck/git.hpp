#pragma once
#include <syrnck/core.hpp>
namespace syrnck::git {
struct Pointer {
  std::string manifest;
  std::uint64_t size;
  std::string encode() const;
};
std::optional<Pointer> parse_pointer(std::string_view);
void clean(const Store &, std::istream &, std::ostream &);
void clean(const Store &, std::istream &, std::ostream &, IngestOptions);
void smudge(const Store &, std::istream &, std::ostream &);
void smudge(const Store &, std::istream &, std::ostream &, OperationOptions);
fs::path common_store(const fs::path &directory);
void install(const fs::path &directory, const fs::path &executable, const fs::path &store);
struct Asset {
  std::string path, blob, manifest;
  std::uint64_t size = 0;
  bool executable = false;
};
struct Catalog {
  std::string commit;
  std::vector<Asset> assets;
  Json json() const;
};
// Reads raw committed Git blobs, never checkout filters or working files.
Catalog catalog(const fs::path &directory, std::string_view revision, OperationOptions = {});
// A deterministic parentless revision containing the catalog's asset paths.
Bytes bundle_bytes(const Catalog &);
ObjectKey bundle_key(const Catalog &);
void verify_assets(const Store &, const Catalog &, OperationOptions = {});
ObjectKey prepare_bundle(const Store &, const Catalog &, OperationOptions = {});
void configure_remote(const fs::path &directory, std::string_view name, std::string_view block_url);
std::string remote_url(const fs::path &directory, std::string_view name);
void install_pre_push(const fs::path &directory, const fs::path &executable, const fs::path &store);
// Parse Git's pre-push input, then enumerate every ancestor of non-deleted tips.
std::vector<std::string> push_commits(const fs::path &directory, std::istream &, OperationOptions = {});
} // namespace syrnck::git
