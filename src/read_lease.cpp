#include "internal.hpp"
#include <algorithm>

namespace syrnck {
namespace {
constexpr std::size_t maximum_leases = 4096;
fs::path directory(const Store &store, bool create = false) {
  auto path = store.root() / "reads";
  auto status = fs::symlink_status(path);
  require(status.type() == fs::file_type::not_found || (fs::is_directory(status) && !fs::is_symlink(status)),
          "invalid read lease directory");
  if (create && status.type() == fs::file_type::not_found) {
    require(fs::create_directory(path), "cannot create read lease directory");
    sync_directory(store.root());
  }
  return path;
}
fs::path path_for(const Store &store, std::string_view id, bool create = false) {
  validate_id(id);
  return directory(store, create) / path_from_utf8(std::string(id) + ".json");
}
void regular(const fs::file_status &status, const fs::path &path) {
  require(fs::is_regular_file(status) && !fs::is_symlink(status), "invalid read lease file: " + utf8(path));
}
void write(const Store &store, const ReadLease &lease, std::stop_token stop = {}) {
  auto data = lease.json().dump();
  auto path = path_for(store, lease.id, true);
  auto status = fs::symlink_status(path);
  if (status.type() != fs::file_type::not_found)
    regular(status, path);
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
    regular(file.symlink_status(), file.path());
    require(canonical, "invalid read lease file: " + utf8(file.path()));
    require(++count <= maximum_leases, "read lease capacity exceeded; prune inactive leases");
    auto lease = store.read_lease(std::string_view(name).substr(0, 64));
    require(lease.has_value(), "read lease disappeared");
    visitor(std::move(*lease));
  }
  throw_if_cancelled(stop);
}
} // namespace
void ReadLease::validate() const {
  validate_id(id);
  validate_id(fence);
  (void)parse_key(key_json(root));
  require(state == "active" || state == "released" || state == "expired", "invalid read lease state");
  require(expires_at > 0 && expires_at <= 253402300799ULL, "invalid read lease expiry");
}
Json ReadLease::json() const {
  validate();
  return Json{
      {"version", 1},  {"id", id}, {"root", key_json(root)}, {"fence", fence}, {"expires_at", expires_at},
      {"state", state}};
}
ReadLease ReadLease::parse(const Json &value) {
  require(value.is_object() && value.size() == 6 && value.contains("version") &&
              value["version"].is_number_integer() && value["version"] == 1 && value.contains("id") &&
              value["id"].is_string() && value.contains("root") && value.contains("fence") &&
              value["fence"].is_string() && value.contains("expires_at") &&
              value["expires_at"].is_number_unsigned() && value.contains("state") &&
              value["state"].is_string(),
          "invalid read lease document");
  ReadLease result;
  result.id = value["id"].get<std::string>();
  result.root = parse_key(value["root"]);
  result.fence = value["fence"].get<std::string>();
  result.expires_at = value["expires_at"].get<std::uint64_t>();
  result.state = value["state"].get<std::string>();
  result.validate();
  return result;
}
std::optional<ReadLease> Store::read_lease(std::string_view id) const {
  auto path = path_for(*this, id);
  auto status = fs::symlink_status(path);
  if (status.type() == fs::file_type::not_found)
    return {};
  regular(status, path);
  auto result = ReadLease::parse(parse_json(read_bytes(path, 16384)));
  require(result.id == id, "read lease identity mismatch");
  return result;
}
std::vector<ReadLease> Store::read_leases(std::stop_token stop) const {
  std::vector<ReadLease> result;
  visit(*this, [&](ReadLease lease) { result.push_back(std::move(lease)); }, stop);
  std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) { return a.id < b.id; });
  throw_if_cancelled(stop);
  return result;
}
ReadLease Store::acquire_read_lease(std::string_view id, const ObjectKey &root, std::uint64_t seconds,
                                    const std::optional<std::string> &fence) const {
  require(seconds >= 1 && seconds <= 86400, "read lease duration must be in 1..86400 seconds");
  validate_id(id);
  (void)parse_key(key_json(root));
  auto previous = read_lease(id);
  if (previous) {
    require(previous->root == root, "read lease descriptor mismatch");
    if ((fence && previous->fence != *fence) || previous->state == "released")
      throw LeaseRejected("read lease is expired, released or fenced");
  } else {
    if (fence)
      throw LeaseRejected("read lease is expired, released or fenced");
    std::size_t count = 0;
    visit(*this, [&](const ReadLease &) { ++count; });
    require(count < maximum_leases, "read lease capacity exceeded; prune inactive leases");
  }
  auto lease = previous.value_or(ReadLease{});
  lease.id = id;
  lease.root = root;
  if (!previous || lease.state == "expired" || lease.expires_at <= unix_seconds()) {
    // Establish availability before acknowledging protection. Store access is serialized by the caller.
    (void)closure({root}, true);
    lease.fence = hex(random_bytes(32));
  }
  lease.state = "active";
  lease.expires_at = unix_seconds() + seconds + 1;
  write(*this, lease);
  return lease;
}
void Store::release_read_lease(std::string_view id, const std::optional<std::string> &fence) const {
  auto lease = read_lease(id);
  if (!lease)
    throw MissingObject("unknown read lease");
  if (fence && *fence != lease->fence)
    throw LeaseRejected("read lease is expired, released or fenced");
  if (lease->state == "released")
    return;
  lease->state = "released";
  write(*this, *lease);
}
std::size_t Store::prune_read_leases() const {
  std::vector<std::string> inactive;
  auto now = unix_seconds();
  visit(*this, [&](const ReadLease &lease) {
    if (lease.state != "active" || lease.expires_at <= now)
      inactive.push_back(lease.id);
  });
  for (const auto &id : inactive)
    require(fs::remove(path_for(*this, id)), "cannot remove read lease");
  if (!inactive.empty())
    sync_directory(directory(*this));
  return inactive.size();
}
void Store::check_read_gc(std::stop_token stop, bool remove) const {
  const auto now = unix_seconds();
  std::vector<ReadLease> expired;
  visit(
      *this,
      [&](const ReadLease &lease) {
        require(lease.state != "active" || lease.expires_at <= now,
                "garbage collection blocked by active read leases");
        if (remove && lease.state == "active")
          expired.push_back(lease);
      },
      stop);
  for (auto &lease : expired) {
    throw_if_cancelled(stop);
    lease.state = "expired";
    lease.fence = hex(random_bytes(32));
    write(*this, lease, stop);
  }
}
} // namespace syrnck
