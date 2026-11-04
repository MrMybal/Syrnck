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
// Called only for missing objects, before any reconstructed bytes are emitted.
using AssetResolver = std::function<void(const Pointer &)>;
void smudge(const Store &, std::istream &, std::ostream &, OperationOptions, const AssetResolver &);
// Preserve canonical pointers and stream ordinary content without opening a Store.
void skip_smudge(std::istream &, std::ostream &, OperationOptions = {});
// Borrowed command/pathname values are valid only during the callback. The
// handler consumes decoded stdin and writes unframed content. Recoverable file
// errors are reported to Git and diagnostics; protocol errors terminate.
using FilterHandler =
    std::function<void(std::string_view command, std::string_view pathname, std::istream &, std::ostream &)>;
void filter_process(std::istream &, std::ostream &, const FilterHandler &, OperationOptions = {},
                    const std::function<void(std::string_view)> &diagnostic = {});
enum class CheckoutMode { local, download, skip };
struct CheckoutPolicy {
  CheckoutMode mode = CheckoutMode::local;
  std::string remote;
};
void configure_checkout(const fs::path &directory, const CheckoutPolicy &);
CheckoutPolicy checkout_policy(const fs::path &directory);
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
struct RetentionInventory {
  std::string repository, fingerprint;
  std::size_t objects = 0;
  std::set<std::string> commits;
  std::vector<Pointer> manifests;
  Json json() const;
};
// Reads all refs, reflogs and indexes across worktrees. Pointer-shaped blobs
// are retained conservatively regardless of their tree mode or attributes.
RetentionInventory retention_inventory(const fs::path &directory, OperationOptions = {});
// Preview by default. Apply adds verified per-repository manifest pins in one
// transaction. Unobserved pins are reported, never removed automatically.
Json reconcile_retention(const Store &, const fs::path &directory, bool apply = false, OperationOptions = {});
void configure_remote(const fs::path &directory, std::string_view name, std::string_view block_url);
std::string remote_url(const fs::path &directory, std::string_view name);
void install_pre_push(const fs::path &directory, const fs::path &executable, const fs::path &store);
// Parse Git's pre-push input, then enumerate every ancestor of non-deleted tips.
std::vector<std::string> push_commits(const fs::path &directory, std::istream &, OperationOptions = {});
} // namespace syrnck::git
