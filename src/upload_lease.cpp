#include "internal.hpp"
#include <algorithm>
#include <chrono>

namespace syrnck {
std::uint64_t unix_seconds() {
  auto now =
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
          .count();
  require(now >= 0, "system clock is before the Unix epoch");
  return static_cast<std::uint64_t>(now);
}
namespace {
constexpr std::size_t maximum_leases = 4096;
fs::path directory(const Store &store, bool create = false) {
  auto path = store.root() / "uploads";
  auto status = fs::symlink_status(path);
  require(status.type() == fs::file_type::not_found || (fs::is_directory(status) && !fs::is_symlink(status)),
          "invalid upload lease directory");
  if (create && status.type() == fs::file_type::not_found) {
    require(fs::create_directory(path), "cannot create upload lease directory");
    sync_directory(store.root());
  }
  return path;
}
fs::path path_for(const Store &store, std::string_view id, bool create = false) {
  validate_id(id);
  return directory(store, create) / path_from_utf8(std::string(id) + ".json");
}
void regular(const fs::file_status &status) {
  require(fs::is_regular_file(status) && !fs::is_symlink(status), "invalid upload lease file");
}
void write(const Store &store, const UploadLease &lease, std::stop_token stop = {}) {
  auto data = lease.json().dump();
  auto path = path_for(store, lease.id, true);
  auto status = fs::symlink_status(path);
  if (status.type() != fs::file_type::not_found)
    regular(status);
  atomic_write(path, bytes(data), status.type() != fs::file_type::not_found, stop);
}
template <class Visitor> void visit(const Store &store, Visitor visitor, std::stop_token stop = {}) {
  throw_if_cancelled(stop);
  auto path = directory(store);
  if (!fs::exists(path))
    return;
  std::size_t count = 0;
  for (const auto &file : fs::directory_iterator(path)) {
    throw_if_cancelled(stop);
    auto name = utf8(file.path().filename());
    if (name.starts_with(".tmp"))
      continue;
    const bool canonical = name.size() == 69 && name.ends_with(".json");
    if (!canonical && transient_metadata_disappeared(file.path(), stop))
      continue;
    regular(file.symlink_status());
    require(canonical, "invalid upload lease file");
    require(++count <= maximum_leases, "upload lease capacity exceeded; prune inactive leases");
    auto lease = store.upload_lease(std::string_view(name).substr(0, 64));
    require(lease.has_value(), "upload lease disappeared");
    visitor(std::move(*lease));
  }
  throw_if_cancelled(stop);
}
UploadLease checked(const Store &store, std::string_view id, std::string_view fence) {
  auto lease = store.upload_lease(id);
  if (!lease)
    throw MissingObject("unknown upload lease");
  if (lease->fence != fence)
    throw LeaseRejected("upload lease is expired, released or fenced");
  return std::move(*lease);
}
} // namespace
void UploadLease::validate() const {
  validate_id(id);
  validate_id(fence);
  validate_ref(reference);
  require(reference.find('/') == reference.npos, "remote reference must be a single component");
  (void)parse_key(key_json(root));
  if (expected)
    (void)parse_key(key_json(*expected));
  require(state == "active" || state == "published" || state == "released" || state == "expired",
          "invalid upload lease state");
  require(expires_at > 0 && expires_at <= 253402300799ULL, "invalid upload lease expiry");
}
Json UploadLease::json() const {
  validate();
  return Json{{"version", 1},
              {"id", id},
              {"reference", reference},
              {"root", key_json(root)},
              {"expected", expected ? key_json(*expected) : Json(nullptr)},
              {"fence", fence},
              {"expires_at", expires_at},
              {"state", state}};
}
UploadLease UploadLease::parse(const Json &value) {
  require(value.is_object() && value.size() == 8 && value.contains("version") &&
              value["version"].is_number_integer() && value["version"] == 1 && value.contains("id") &&
              value["id"].is_string() && value.contains("reference") && value["reference"].is_string() &&
              value.contains("root") && value.contains("expected") && value.contains("fence") &&
              value["fence"].is_string() && value.contains("expires_at") &&
              value["expires_at"].is_number_unsigned() && value.contains("state") &&
              value["state"].is_string(),
          "invalid upload lease document");
  UploadLease result;
  result.id = value["id"].get<std::string>();
  result.reference = value["reference"].get<std::string>();
  result.root = parse_key(value["root"]);
  if (!value["expected"].is_null())
    result.expected = parse_key(value["expected"]);
  result.fence = value["fence"].get<std::string>();
  result.expires_at = value["expires_at"].get<std::uint64_t>();
  result.state = value["state"].get<std::string>();
  result.validate();
  return result;
}
std::optional<UploadLease> Store::upload_lease(std::string_view id) const {
  auto path = path_for(*this, id);
  auto status = fs::symlink_status(path);
  if (status.type() == fs::file_type::not_found)
    return {};
  regular(status);
  auto result = UploadLease::parse(parse_json(read_bytes(path, 16384)));
  require(result.id == id, "upload lease identity mismatch");
  return result;
}
std::vector<UploadLease> Store::upload_leases(std::stop_token stop) const {
  std::vector<UploadLease> result;
  visit(*this, [&](UploadLease lease) { result.push_back(std::move(lease)); }, stop);
  std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) { return a.id < b.id; });
  throw_if_cancelled(stop);
  return result;
}
UploadLease Store::acquire_upload_lease(std::string_view id, const ObjectKey &root, std::string_view name,
                                        const std::optional<ObjectKey> &expected, std::uint64_t seconds,
                                        const std::optional<std::string> &fence) const {
  require(seconds >= 1 && seconds <= 86400, "upload lease duration must be in 1..86400 seconds");
  validate_id(id);
  auto previous = upload_lease(id);
  UploadLease lease;
  if (previous) {
    lease = *previous;
    require(lease.root == root && lease.reference == name && lease.expected == expected,
            "upload lease descriptor mismatch");
    if ((fence && lease.fence != *fence) || lease.state == "released")
      throw LeaseRejected("upload lease is expired, released or fenced");
    if (lease.state == "published")
      return lease;
  } else {
    if (fence)
      throw LeaseRejected("upload lease is expired, released or fenced");
    std::size_t count = 0;
    visit(*this, [&](const UploadLease &) { ++count; });
    require(count < maximum_leases, "upload lease capacity exceeded; prune inactive leases");
    lease.id = id;
    lease.root = root;
    lease.reference = name;
    lease.expected = expected;
  }
  auto now = unix_seconds();
  if (!previous || lease.state == "expired" || lease.expires_at <= now)
    lease.fence = hex(random_bytes(32));
  lease.state = "active";
  lease.expires_at = now + seconds + 1; // Round the whole-second expiry up, never shorten a one-second TTL.
  write(*this, lease);
  return lease;
}
bool Store::publish_upload_lease(std::string_view id, std::string_view fence) const {
  auto lease = checked(*this, id, fence);
  if (lease.state == "published")
    return true; // A receipt never republishes a reference changed later.
  if (lease.state == "active" && lease.expires_at <= unix_seconds()) {
    lease.state = "expired";
    lease.fence = hex(random_bytes(32));
    write(*this, lease);
  }
  if (lease.state != "active")
    throw LeaseRejected("upload lease is expired, released or fenced");
  if (!compare_set_ref(lease.reference, lease.expected, lease.root))
    return false;
  lease.state = "published";
  write(*this, lease);
  return true;
}
void Store::confirm_upload_lease(std::string_view id, std::string_view fence) const {
  auto lease = checked(*this, id, fence);
  if (lease.state == "published")
    return;
  auto all = refs();
  if (!all.contains(lease.reference) || all.at(lease.reference) != lease.root)
    throw LeaseRejected("upload lease reference does not match its root");
  (void)closure({lease.root}, true);
  lease.state = "published";
  write(*this, lease);
}
void Store::release_upload_lease(std::string_view id) const {
  auto lease = upload_lease(id);
  require(lease.has_value(), "unknown upload lease");
  if (lease->state != "active")
    return;
  lease->state = "released";
  write(*this, *lease);
}
std::size_t Store::prune_upload_leases() const {
  std::vector<std::string> inactive;
  auto now = unix_seconds();
  visit(*this, [&](const UploadLease &lease) {
    if (lease.state != "active" || lease.expires_at <= now)
      inactive.push_back(lease.id);
  });
  for (const auto &id : inactive)
    require(fs::remove(path_for(*this, id)), "cannot remove upload lease");
  if (!inactive.empty())
    sync_directory(directory(*this));
  return inactive.size();
}
void Store::check_upload_gc(std::stop_token stop, bool remove) const {
  const auto now = unix_seconds();
  std::vector<UploadLease> expired;
  visit(
      *this,
      [&](const UploadLease &lease) {
        require(lease.state != "active" || lease.expires_at <= now,
                "garbage collection blocked by active upload leases");
        if (remove && lease.state == "active")
          expired.push_back(lease);
      },
      stop);
  // Persist observed expiry before deleting anything. A later wall-clock rollback
  // must not resurrect a fence whose negotiated objects GC may have removed.
  for (auto &lease : expired) {
    throw_if_cancelled(stop);
    lease.state = "expired";
    lease.fence = hex(random_bytes(32));
    write(*this, lease, stop);
  }
}
} // namespace syrnck
