#pragma once
#include <syrnck/git.hpp>

namespace syrnck::git {
struct LfsPublicationUncertain : Error {
  using Error::Error;
};
struct LfsPointer {
  std::string oid;
  std::uint64_t size = 0;
  std::string encode() const;
};
std::optional<LfsPointer> parse_lfs_pointer(std::string_view);
struct LfsAsset {
  std::string path, blob;
  LfsPointer pointer;
  bool executable = false;
};
struct LfsCatalog {
  std::string commit;
  std::vector<LfsAsset> assets;
  Json json() const;
};
LfsCatalog lfs_catalog(const fs::path &, std::string_view revision, OperationOptions = {});
fs::path lfs_objects(const fs::path &directory, const std::optional<fs::path> &override_path = {});
// Read-only SHA-256 audit; missing/corrupt objects are reported in English JSON.
Json preview_lfs(const LfsCatalog &, const fs::path &objects, OperationOptions = {});
// Creates one child commit and a new branch, leaving HEAD/index/worktree untouched.
Json migrate_lfs(const Store &, const fs::path &directory, std::string_view revision, const fs::path &objects,
                 std::string_view new_branch, IngestOptions = {});
} // namespace syrnck::git
