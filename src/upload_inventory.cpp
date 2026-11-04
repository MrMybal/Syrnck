#include "upload_inventory.hpp"
#include <algorithm>
namespace syrnck::upload_inventory {
namespace {
fs::path directory(const Store &store, bool create = false) {
  auto path = store.root() / "upload-inventories";
  auto status = fs::symlink_status(path);
  require(status.type() == fs::file_type::not_found || (fs::is_directory(status) && !fs::is_symlink(status)),
          "invalid upload inventory directory");
  if (create && status.type() == fs::file_type::not_found) {
    require(fs::create_directory(path), "cannot create upload inventory directory");
    sync_directory(store.root());
  }
  return path;
}
fs::path generation_path(const Store &store, const UploadLease &lease, bool create = false) {
  auto parent = directory(store, create);
  auto path = parent / generation(lease);
  auto status = fs::symlink_status(path);
  require(status.type() == fs::file_type::not_found || (fs::is_directory(status) && !fs::is_symlink(status)),
          "invalid upload inventory directory");
  if (create && status.type() == fs::file_type::not_found) {
    require(fs::create_directory(path), "cannot create upload inventory directory");
    sync_directory(parent);
  }
  return path;
}
fs::path page_path(const Store &store, const UploadLease &lease, std::uint32_t index, bool create = false) {
  require(index < max_pages, "upload inventory page limit exceeded");
  auto name = std::to_string(index);
  return generation_path(store, lease, create) / (std::string(8 - name.size(), '0') + name + ".json");
}
void regular(const fs::file_status &status) {
  require(fs::is_regular_file(status) && !fs::is_symlink(status), "invalid upload inventory file");
}
void validate_objects(const std::vector<ObjectKey> &objects) {
  require(!objects.empty() && objects.size() <= max_objects, "invalid upload inventory objects");
  for (std::size_t i = 0; i < objects.size(); ++i) {
    (void)parse_key(key_json(objects[i]));
    require(i == 0 || objects[i - 1] < objects[i], "upload inventory objects must be strictly ordered");
  }
}
Json document(const UploadLease &lease, std::uint32_t index, std::string_view previous,
              const std::vector<ObjectKey> &objects) {
  Json entries = Json::array();
  for (const auto &key : objects)
    entries.push_back(key_json(key));
  return Json{{"version", 1},   {"lease", lease.id},    {"fence", lease.fence},
              {"index", index}, {"previous", previous}, {"objects", std::move(entries)}};
}
template <class Visitor> void generations(const Store &store, Visitor visitor) {
  auto parent = directory(store);
  if (!fs::exists(parent))
    return;
  for (const auto &entry : fs::directory_iterator(parent)) {
    auto name = utf8(entry.path().filename());
    validate_id(name);
    auto status = entry.symlink_status();
    require(fs::is_directory(status) && !fs::is_symlink(status), "invalid upload inventory directory");
    visitor(entry.path(), name);
  }
}
template <class Visitor> void files(const fs::path &path, Visitor visitor) {
  for (const auto &entry : fs::directory_iterator(path)) {
    auto name = utf8(entry.path().filename());
    const bool page =
        name.size() == 13 && name.ends_with(".json") &&
        std::all_of(name.begin(), name.begin() + 8, [](char c) { return c >= '0' && c <= '9'; });
    if (!page && !name.starts_with(".tmp") && transient_metadata_disappeared(entry.path()))
      continue;
    regular(entry.symlink_status());
    require(page || name.starts_with(".tmp"), "invalid upload inventory file");
    visitor(entry.path());
  }
}
} // namespace
std::string generation(const UploadLease &lease) {
  validate_id(lease.id);
  validate_id(lease.fence);
  return hash(bytes(Json::array({lease.id, lease.fence}).dump()));
}
std::string seed(const UploadLease &lease) {
  return hash(bytes(Json::array({"syrnck-upload-inventory-v1", lease.id, lease.fence}).dump()));
}
namespace {
struct Page {
  std::vector<ObjectKey> objects;
  std::string previous, checksum;
};
Page read_data(const Store &store, const UploadLease &lease, std::uint32_t index) {
  auto path = page_path(store, lease, index);
  regular(fs::symlink_status(path));
  auto value = parse_json(read_bytes(path, max_page_bytes));
  require(value.is_object() && value.size() == 7 && value.contains("version") &&
              value["version"].is_number_integer() && value["version"] == 1 && value.contains("lease") &&
              value["lease"].is_string() && value["lease"] == lease.id && value.contains("fence") &&
              value["fence"].is_string() && value["fence"] == lease.fence && value.contains("index") &&
              value["index"].is_number_unsigned() && value["index"] == index && value.contains("objects") &&
              value["objects"].is_array() && !value["objects"].empty() &&
              value["objects"].size() <= max_objects && value.contains("checksum") &&
              value["checksum"].is_string() && value.contains("previous") && value["previous"].is_string(),
          "invalid upload inventory page");
  std::vector<ObjectKey> objects;
  objects.reserve(value["objects"].size());
  for (const auto &item : value["objects"])
    objects.push_back(parse_key(item));
  validate_objects(objects);
  auto previous = value["previous"].get<std::string>();
  validate_id(previous);
  auto checksum = hash(bytes(document(lease, index, previous, objects).dump()));
  require(value["checksum"] == checksum, "upload inventory checksum mismatch");
  return {std::move(objects), std::move(previous), std::move(checksum)};
}
} // namespace
std::vector<ObjectKey> read_page(const Store &store, const UploadLease &lease, std::uint32_t index) {
  return read_data(store, lease, index).objects;
}
std::string write_page(const Store &store, const UploadLease &lease, std::uint32_t index,
                       const std::vector<ObjectKey> &objects) {
  require(index < max_pages, "upload inventory page limit exceeded");
  validate_objects(objects);
  if (index) {
    auto previous = read_data(store, lease, index - 1);
    require(previous.checksum == lease.inventory_hash, "upload inventory checksum mismatch");
    require(previous.objects.back() < objects.front(), "upload inventory objects must be strictly ordered");
  } else
    require(lease.inventory_hash == seed(lease), "upload inventory checksum mismatch");
  auto path = page_path(store, lease, index, true);
  if (fs::symlink_status(path).type() != fs::file_type::not_found) {
    auto existing = read_data(store, lease, index);
    require(existing.objects == objects && existing.previous == lease.inventory_hash,
            "upload inventory page conflicts with its previous value");
    return existing.checksum;
  }
  auto value = document(lease, index, lease.inventory_hash, objects);
  auto checksum = hash(bytes(value.dump()));
  value["checksum"] = checksum;
  auto data = value.dump();
  require(data.size() <= max_page_bytes, "upload inventory page exceeds size limit");
  atomic_write(path, bytes(data), false);
  return checksum;
}
void visit(const Store &store, const UploadLease &lease, const std::function<void(const ObjectKey &)> &emit,
           std::stop_token stop) {
  require(lease.inventory_pages.has_value(), "upload lease has no selective inventory");
  std::optional<ObjectKey> previous;
  auto expected = seed(lease);
  for (std::uint32_t i = 0; i < *lease.inventory_pages; ++i) {
    throw_if_cancelled(stop);
    auto page = read_data(store, lease, i);
    require(page.previous == expected, "upload inventory checksum mismatch");
    expected = page.checksum;
    const auto &objects = page.objects;
    require(!previous || *previous < objects.front(), "upload inventory objects must be strictly ordered");
    previous = objects.back();
    for (const auto &key : objects) {
      throw_if_cancelled(stop);
      emit(key);
    }
  }
  require(expected == lease.inventory_hash, "upload inventory checksum mismatch");
  throw_if_cancelled(stop);
}
void check_tree(const Store &store) {
  generations(store,
              [&](const fs::path &path, const std::string &) { files(path, [](const fs::path &) {}); });
}
void remove_inactive(const Store &store, const std::set<std::string> &live) {
  bool changed = false;
  generations(store, [&](const fs::path &path, const std::string &name) {
    if (live.contains(name))
      return;
    // Each validated path is a direct regular child of this named metadata directory.
    files(path,
          [&](const fs::path &file) { require(fs::remove(file), "cannot remove upload inventory file"); });
    require(fs::remove(path), "cannot remove upload inventory directory");
    changed = true;
  });
  if (changed)
    sync_directory(directory(store));
}
} // namespace syrnck::upload_inventory
