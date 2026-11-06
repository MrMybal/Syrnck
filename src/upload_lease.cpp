#include "internal.hpp"
#include "upload_inventory.hpp"
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
  require(!inventory_pages || *inventory_pages <= upload_inventory::max_pages,
          "upload inventory page limit exceeded");
  if (inventory_pages)
    validate_id(inventory_hash);
  else
    require(inventory_hash.empty(), "invalid upload lease document");
}
Json UploadLease::json() const {
  validate();
  auto value = Json{{"version", inventory_pages ? 2 : 1},
                    {"id", id},
                    {"reference", reference},
                    {"root", key_json(root)},
                    {"expected", expected ? key_json(*expected) : Json(nullptr)},
                    {"fence", fence},
                    {"expires_at", expires_at},
                    {"state", state}};
  if (inventory_pages) {
    value["inventory_pages"] = *inventory_pages;
    value["inventory_hash"] = inventory_hash;
  }
  return value;
}
UploadLease UploadLease::parse(const Json &value) {
  require(value.is_object() && value.contains("version") && value["version"].is_number_integer() &&
              ((value["version"] == 1 && value.size() == 8) ||
               (value["version"] == 2 && value.size() == 10 && value.contains("inventory_pages") &&
                value.contains("inventory_hash") && value["inventory_hash"].is_string() &&
                value["inventory_pages"].is_number_unsigned() &&
                value["inventory_pages"] <= upload_inventory::max_pages)) &&
              value.contains("id") && value["id"].is_string() && value.contains("reference") &&
              value["reference"].is_string() && value.contains("root") && value.contains("expected") &&
              value.contains("fence") && value["fence"].is_string() && value.contains("expires_at") &&
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
  if (value["version"] == 2) {
    result.inventory_pages = value["inventory_pages"].get<std::uint32_t>();
    result.inventory_hash = value["inventory_hash"].get<std::string>();
  }
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
                                        const std::optional<std::string> &fence, bool inventory) const {
  require(seconds >= 1 && seconds <= 86400, "upload lease duration must be in 1..86400 seconds");
  validate_id(id);
  auto previous = upload_lease(id);
  UploadLease lease;
  if (previous) {
    lease = *previous;
    require(lease.root == root && lease.reference == name && lease.expected == expected &&
                lease.inventory_pages.has_value() == inventory,
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
    if (inventory)
      lease.inventory_pages = 0;
  }
  auto now = unix_seconds();
  if (!previous || lease.state == "expired" || lease.expires_at <= now) {
    lease.fence = hex(random_bytes(32));
    if (lease.inventory_pages) {
      lease.inventory_pages = 0;
      lease.inventory_hash = upload_inventory::seed(lease);
    }
  }
  lease.state = "active";
  lease.expires_at = now + seconds + 1; // Round the whole-second expiry up, never shorten a one-second TTL.
  write(*this, lease);
  return lease;
}
void Store::register_upload_inventory(std::string_view id, std::string_view fence, std::uint32_t page,
                                      const std::vector<ObjectKey> &objects) const {
  auto lease = checked(*this, id, fence);
  if (lease.state == "active" && lease.expires_at <= unix_seconds()) {
    lease.state = "expired";
    lease.fence = hex(random_bytes(32));
    write(*this, lease);
  }
  if (lease.state != "active")
    throw LeaseRejected("upload lease is expired, released or fenced");
  require(lease.inventory_pages.has_value(), "upload lease has no selective inventory");
  require(page <= *lease.inventory_pages, "upload inventory page is out of sequence");
  if (page < *lease.inventory_pages) {
    require(upload_inventory::read_page(*this, lease, page) == objects,
            "upload inventory page conflicts with its previous value");
    return;
  }
  auto checksum = upload_inventory::write_page(*this, lease, page, objects);
  if (page == *lease.inventory_pages) {
    ++*lease.inventory_pages;
    lease.inventory_hash = std::move(checksum);
    write(*this, lease); // Commit the page before acknowledging it to the sender.
  }
}
bool Store::publish_upload_lease(std::string_view id, std::string_view fence,
                                 OperationOptions options) const {
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
  const auto observer = options.progress;
  options.progress = [&, observer](const ProgressEvent &event) {
    if (observer)
      observer(event);
    if (event.phase == ProgressPhase::publishing && lease.expires_at <= unix_seconds())
      throw LeaseRejected("upload lease expired before publication");
  };
  if (!compare_update_refs({{lease.reference, lease.expected, lease.root}}, options))
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
  std::set<std::string> live;
  auto now = unix_seconds();
  visit(*this, [&](const UploadLease &lease) {
    if (lease.state != "active" || lease.expires_at <= now)
      inactive.push_back(lease.id);
    else if (lease.inventory_pages) {
      live.insert(upload_inventory::generation(lease));
      upload_inventory::visit(*this, lease, [](const ObjectKey &) {}, {});
    }
  });
  upload_inventory::check_tree(*this);
  for (const auto &id : inactive)
    require(fs::remove(path_for(*this, id)), "cannot remove upload lease");
  if (!inactive.empty())
    sync_directory(directory(*this));
  upload_inventory::remove_inactive(*this, live);
  return inactive.size();
}
std::vector<UploadLease> Store::prepare_upload_gc(std::stop_token stop, bool remove) const {
  const auto now = unix_seconds();
  std::vector<UploadLease> expired;
  std::vector<UploadLease> active;
  visit(
      *this,
      [&](const UploadLease &lease) {
        if (lease.state == "active" && lease.expires_at > now) {
          require(lease.inventory_pages.has_value(), "garbage collection blocked by active upload leases");
          active.push_back(lease);
        } else if (remove && lease.state == "active")
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
  return active;
}
void Store::visit_upload_inventory(const UploadLease &lease,
                                   const std::function<void(const ObjectKey &)> &visitor,
                                   std::stop_token stop) const {
  upload_inventory::visit(*this, lease, visitor, stop);
}
} // namespace syrnck
