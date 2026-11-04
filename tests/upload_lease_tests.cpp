#include "internal.hpp"
#include <fstream>
#include <iostream>
using namespace syrnck;
namespace {
void check(bool value) {
  require(value, "upload lease test failed");
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
void expire(const Store &store, const UploadLease &lease) {
  auto record = lease.json();
  record["expires_at"] = std::uint64_t{1};
  atomic_write(store.root() / "uploads" / (lease.id + ".json"), bytes(record.dump()), true);
}
} // namespace
int main() {
  auto root = fs::temp_directory_path() / path_from_utf8("syrnck-leases-" + hex(random_bytes(16)));
  fs::create_directory(root);
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code ec;
      fs::remove_all(path, ec);
    }
  } cleanup{root};
  try {
    for (bool encrypted : {false, true}) {
      std::optional<Key> key;
      if (encrypted) {
        key.emplace();
        key->fill(0x46);
      }
      auto directory = root / (encrypted ? "encrypted" : "plain");
      auto id = hex(random_bytes(32));
      std::string fence;
      ObjectKey object;
      {
        Store store(directory, key, true);
        check(store.upload_leases().empty());
        object = store.put(Kind::chunk, bytes("pending upload")).first;
        auto lease = store.acquire_upload_lease(id, object, "asset", {}, 3600);
        fence = lease.fence;
        check(UploadLease::parse(lease.json()).json() == lease.json());
        auto renewed = store.acquire_upload_lease(id, object, "asset", {}, 3600, fence);
        check(renewed.fence == fence);
        fails([&] { store.acquire_upload_lease(id, object, "other", {}, 3600); });
        fails([&] { store.acquire_upload_lease(id, object, "asset", {}, 0); });
        fails([&] { store.acquire_upload_lease(id, object, "asset", {}, 86401); });
        fails([&] { store.acquire_upload_lease(id, object, "asset", {}, 3600, std::string(64, '0')); });
        for (auto bad : {Json{{"version", 2}}, Json{{"expires_at", -1}}, Json{{"expires_at", true}},
                         Json{{"state", "invalid"}}, Json{{"fence", "invalid"}}, Json{{"extra", 1}}}) {
          auto value = lease.json();
          value.update(bad);
          fails([&] { (void)UploadLease::parse(value); });
        }
        fails([&] { store.gc(); });
        fails([&] { store.gc(true); });
        check(store.prune_upload_leases() == 0);
      }
      {
        Store store(directory, key);
        auto original = *store.upload_lease(id);
        check(original.fence == fence && store.upload_leases().size() == 1);
        fails([&] { store.gc(true); });
        expire(store, original);
        fails([&] { store.publish_upload_lease(id, fence); });
        auto active =
            store.acquire_upload_lease(id, object, "asset", {}, 3600, store.upload_lease(id)->fence);
        check(active.fence != fence);
        fails([&] { store.publish_upload_lease(id, fence); });
        check(store.refs().empty());
        check(store.publish_upload_lease(id, active.fence));
        check(store.upload_lease(id)->state == "published");
        store.remove_ref("asset");
        auto newer = store.put(Kind::chunk, bytes("newer reference")).first;
        store.set_ref("asset", newer);
        check(store.publish_upload_lease(id, active.fence) && store.refs().at("asset") == newer);
        store.release_upload_lease(id);
        check(store.upload_lease(id)->state == "published");
        check(store.gc(true)["unreachable_objects"] == 1);
        check(store.prune_upload_leases() == 1);
        auto gap = store.acquire_upload_lease(id, newer, "gap", {}, 3600);
        expire(store, gap);
        store.gc(true);
        auto observed = *store.upload_lease(id);
        check(observed.state == "expired" && observed.fence != gap.fence);
        // Model an expiry now lying in the future after clock rollback. The
        // persisted state still forbids publication and requires a fresh fence.
        observed.expires_at = unix_seconds() + 3600;
        atomic_write(store.root() / "uploads" / (id + ".json"), bytes(observed.json().dump()), true);
        fails([&] { store.publish_upload_lease(id, gap.fence); });
        fails([&] { store.publish_upload_lease(id, observed.fence); });
        auto resumed = store.acquire_upload_lease(id, newer, "gap", {}, 3600, observed.fence);
        check(resumed.state == "active" && resumed.fence != observed.fence);
        store.release_upload_lease(id);
        store.prune_upload_leases();
        // A removed/recreated ID gets a fresh fence, preventing an ABA publish.
        auto fresh = store.acquire_upload_lease(id, newer, "other", {}, 3600);
        check(fresh.fence != active.fence);
        fails([&] { store.publish_upload_lease(id, active.fence); });
        store.release_upload_lease(id);
        fails([&] { store.acquire_upload_lease(id, newer, "other", {}, 3600); });
        check(store.gc(true)["live_objects"] == 1);
        check(store.prune_upload_leases() == 1);
        auto confirmation = store.acquire_upload_lease(id, newer, "asset", {}, 3600);
        store.confirm_upload_lease(id, confirmation.fence);
        check(store.upload_lease(id)->state == "published");
        store.prune_upload_leases();
        auto expired = store.acquire_upload_lease(id, newer, "other", {}, 3600);
        expire(store, expired);
        check(store.gc()["live_objects"] == 1 && store.prune_upload_leases() == 1);
        // Corrupt journals fail closed; pruning must validate all records first.
        auto path = store.root() / "uploads" / (id + ".json");
        atomic_write(path, bytes("{"), false);
        fails([&] { store.gc(true); });
        fails([&] { store.prune_upload_leases(); });
        fs::remove(path);
        fails([&] { store.upload_lease("../refs.json"); });
        std::stop_source stop;
        stop.request_stop();
        fails([&] { store.upload_leases(stop.get_token()); });
      }
    }
    std::cout << "Upload leases: reopen, expiry, fencing, receipts, discard and malformed records passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
