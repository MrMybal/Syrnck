#include "upload_inventory.hpp"
#include <algorithm>
#include <iostream>
#include <sstream>
#include <source_location>
using namespace syrnck;
namespace inv = syrnck::upload_inventory;
namespace {
void check(bool value, std::source_location where = std::source_location::current()) {
  require(value, "upload inventory test failed at line " + std::to_string(where.line()));
}
template <class Function> void fails(Function function) {
  bool failed = false;
  try {
    function();
  } catch (const std::exception &) {
    failed = true;
  }
  check(failed);
}
fs::path page(const Store &store, const UploadLease &lease, std::uint32_t index = 0) {
  auto name = std::to_string(index);
  return store.root() / "upload-inventories" / inv::generation(lease) /
         (std::string(8 - name.size(), '0') + name + ".json");
}
void expire(const Store &store, const UploadLease &lease) {
  auto value = lease.json();
  value["expires_at"] = std::uint64_t{1};
  atomic_write(store.root() / "uploads" / (lease.id + ".json"), bytes(value.dump()), true);
}
} // namespace
int main() {
  auto root = fs::temp_directory_path() / path_from_utf8("syrnck-inventory-" + hex(random_bytes(16)));
  fs::create_directory(root);
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code ec;
      fs::remove_all(path, ec);
    }
  } cleanup{root};
  try {
    Store source(root / "source", {}, true);
    std::istringstream input("aaaabbbbcccc");
    ObjectKey manifest{Kind::manifest, source.ingest(input, Chunking::fixed_size(4)).first};
    auto closure = source.closure({manifest}, true);
    std::vector<ObjectKey> all(closure.begin(), closure.end());
    check(all.size() == 4);
    for (bool encrypted : {false, true})
      for (bool packed : {false, true}) {
        std::optional<Key> key;
        if (encrypted) {
          key.emplace();
          key->fill(0x39);
        }
        auto directory =
            root / (std::string(encrypted ? "encrypted-" : "plain-") + (packed ? "packed" : "loose"));
        auto id = hex(random_bytes(32));
        std::string fence;
        std::vector<ObjectKey> first(all.begin(), all.begin() + 2), last(all.begin() + 2, all.end());
        {
          Store store(directory, key, true);
          auto lease = store.acquire_upload_lease(id, manifest, "asset", {}, 3600, {}, true);
          fence = lease.fence;
          check(lease.inventory_pages == 0 && lease.json()["version"] == 2);
          check(UploadLease::parse(lease.json()).json() == lease.json());
          fails([&] { store.acquire_upload_lease(id, manifest, "asset", {}, 3600); }); // Mode cannot change.
          store.register_upload_inventory(id, fence, 0, first);
          check(store.upload_lease(id)->inventory_pages == 1);
          check(store.gc(true)["live_objects"] == 0); // Announced but not received yet.
          for (std::size_t i = 0; i < 3; ++i)
            store.import(all[i], source.get(all[i]));
          auto orphan = store.put(Kind::chunk, bytes("unannounced garbage")).first;
          if (packed)
            store.pack();
          check(!store.contains(manifest));
          auto preview = store.gc();
          check(preview["live_objects"] == 2 && preview["unreachable_objects"] == 2);
          check(store.contains(orphan));
          std::stop_source stop;
          fails([&] {
            store.gc(true, {stop.get_token(), [&](const ProgressEvent &event) {
                              if (event.phase == ProgressPhase::publishing)
                                stop.request_stop();
                            }});
          });
          check(store.contains(orphan));
          check(store.gc(true)["unreachable_objects"] == 2);
          check(!store.contains(orphan) && !store.contains(all[2]));
        }
        {
          Store store(directory, key);
          auto lease = *store.upload_lease(id);
          check(lease.fence == fence && lease.inventory_pages == 1);
          for (const auto &object : first)
            check(store.get(object) == source.get(object));
          store.register_upload_inventory(id, fence, 0, first); // Lost ACK retry is idempotent.
          check(store.upload_lease(id)->inventory_pages == 1);
          fails([&] { store.register_upload_inventory(id, fence, 0, {all[0]}); });
          fails([&] { store.register_upload_inventory(id, fence, 2, last); });
          fails([&] { store.register_upload_inventory(id, fence, 1, first); });
          fails([&] { store.register_upload_inventory(id, fence, 1, {}); });
          fails([&] { store.register_upload_inventory(id, std::string(64, '0'), 1, last); });
          auto record_path = store.root() / "uploads" / (id + ".json");
          auto original_record = read_bytes(record_path);
          auto truncated_count = parse_json(original_record);
          truncated_count["inventory_pages"] = std::uint32_t{0};
          atomic_write(record_path, bytes(truncated_count.dump()), true);
          fails([&] { store.gc(true); });
          fails([&] { store.register_upload_inventory(id, fence, 0, first); });
          atomic_write(record_path, original_record, true);
          // A valid-looking key alteration must be caught by the committed page checksum.
          auto original = read_bytes(page(store, lease));
          auto corrupt = parse_json(original);
          corrupt["objects"][0]["id"] = std::string(64, '0');
          atomic_write(page(store, lease), bytes(corrupt.dump()), true);
          auto orphan = store.put(Kind::chunk, bytes("preserve on damaged inventory")).first;
          fails([&] { store.gc(true); });
          fails([&] { store.prune_upload_leases(); });
          check(store.contains(orphan));
          atomic_write(page(store, lease), original, true);
          store.register_upload_inventory(id, fence, 1, last);
          for (const auto &object : last)
            store.import(object, source.get(object));
          check(store.gc(true)["unreachable_objects"] == 1);
          check(store.upload_lease(id)->inventory_pages == 2);
          std::ostringstream output;
          store.reconstruct(manifest.id, output);
          check(output.str() == "aaaabbbbcccc");
          check(store.publish_upload_lease(id, fence));
          check(store.refs().at("asset") == manifest);
          store.remove_ref("asset");
          check(store.gc(true)["unreachable_objects"] ==
                4); // A receipt and its pages do not retain payloads.
          check(store.prune_upload_leases() == 1);
          check(fs::is_empty(store.root() / "upload-inventories"));
          auto fresh = store.acquire_upload_lease(id, manifest, "asset", {}, 3600, {}, true);
          check(fresh.fence != fence && fresh.inventory_pages == 0);
          fails([&] { store.register_upload_inventory(id, fence, 0, first); });
          store.register_upload_inventory(id, fresh.fence, 0, {all[0]});
          store.import(all[0], source.get(all[0]));
          auto old_generation = inv::generation(fresh);
          expire(store, *store.upload_lease(id));
          check(store.gc(true)["unreachable_objects"] == 1);
          auto expired = *store.upload_lease(id);
          check(expired.state == "expired" && expired.fence != fresh.fence);
          expired.expires_at = unix_seconds() + 3600; // Rollback does not reactivate the old generation.
          atomic_write(store.root() / "uploads" / (id + ".json"), bytes(expired.json().dump()), true);
          auto resumed = store.acquire_upload_lease(id, manifest, "asset", {}, 3600, expired.fence, true);
          check(resumed.inventory_pages == 0 && inv::generation(resumed) != old_generation);
          store.register_upload_inventory(id, resumed.fence, 0, first);
          check(store.prune_upload_leases() == 0);
          check(!fs::exists(store.root() / "upload-inventories" / old_generation));
          check(fs::exists(page(store, resumed)));
          store.release_upload_lease(id);
          check(store.prune_upload_leases() == 1);
        }
      }
    {
      Store store(root / "overlapping-roots", {}, true);
      auto save_pull = [&](const ObjectKey &object, std::string name) {
        TransferSession session;
        session.direction = "pull";
        session.endpoint = hash(bytes("overlapping retention sources"));
        session.reference = name;
        session.root = object;
        session.id = TransferSession::identity("pull", session.endpoint, object, name);
        store.write_transfer_session(session);
      };
      store.import(manifest, source.get(manifest));
      store.import(all[0], source.get(all[0]));
      store.import(all[2], source.get(all[2]));
      save_pull(manifest, "partial");
      auto lease = store.acquire_upload_lease(hex(random_bytes(32)), manifest, "asset", {}, 3600, {}, true);
      store.register_upload_inventory(lease.id, lease.fence, 0, {manifest});
      store.put(Kind::chunk, bytes("unrelated overlapping-root garbage"));
      auto result = store.gc(true);
      check(result["live_objects"] == 3 && result["unreachable_objects"] == 1);
      check(store.get(all[0]) == source.get(all[0]) && store.get(all[2]) == source.get(all[2]));
      // Direct inventory protection must not suppress a complete pull's file-hash check either.
      store.import(all[1], source.get(all[1]));
      auto bad = source.metadata(Kind::manifest, manifest.id);
      bad["file_hash"] = std::string(64, '0');
      auto invalid = store.put(Kind::manifest, bytes(bad.dump())).first;
      save_pull(invalid, "invalid");
      auto other = store.acquire_upload_lease(hex(random_bytes(32)), invalid, "other", {}, 3600, {}, true);
      store.register_upload_inventory(other.id, other.fence, 0, {invalid});
      auto orphan = store.put(Kind::chunk, bytes("preserve on overlapping validation failure")).first;
      fails([&] { store.gc(true); });
      check(store.contains(orphan));
    }
    {
      Store store(root / "counter-gap", {}, true);
      auto lease = store.acquire_upload_lease(hex(random_bytes(32)), manifest, "asset", {}, 3600, {}, true);
      // Model termination after the immutable page write, before committing its page count.
      inv::write_page(store, lease, 0, {all[0]});
      store.import(all[0], source.get(all[0]));
      check(store.gc(true)["unreachable_objects"] == 1); // Unacknowledged staging is not a hold.
      store.register_upload_inventory(lease.id, lease.fence, 0, {all[0]});
      store.import(all[0], source.get(all[0]));
      check(store.gc(true)["live_objects"] == 1);
      auto bad = lease.json();
      bad["inventory_pages"] = std::uint64_t{4097};
      fails([&] { UploadLease::parse(bad); });
      auto live_page = page(store, lease);
      auto original = read_bytes(live_page);
      fs::remove(live_page);
      auto orphan = store.put(Kind::chunk, bytes("preserve on missing page")).first;
      fails([&] { store.gc(true); });
      fails([&] { store.register_upload_inventory(lease.id, lease.fence, 0, {all[0]}); });
      check(store.contains(orphan));
      atomic_write(live_page, original, false);
    }
    {
      Store store(root / "limits", {}, true);
      auto lease = store.acquire_upload_lease(hex(random_bytes(32)), manifest, "asset", {}, 3600, {}, true);
      std::vector<ObjectKey> objects;
      for (std::size_t i = 0; i < inv::max_objects; ++i)
        objects.push_back({Kind::chunk, hash(bytes("announced-" + std::to_string(i)))});
      std::sort(objects.begin(), objects.end());
      store.register_upload_inventory(lease.id, lease.fence, 0, objects);
      check(inv::read_page(store, lease, 0) == objects);
      objects.push_back(all[0]);
      fails([&] { store.register_upload_inventory(lease.id, lease.fence, 1, objects); });
      fails([&] { store.register_upload_inventory("../uploads", lease.fence, 0, {all[0]}); });
      fails([&] { inv::write_page(store, lease, inv::max_pages, {all[0]}); });
    }
    std::cout << "Upload inventories: selective GC, retries, expiry, corruption and pruning passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
