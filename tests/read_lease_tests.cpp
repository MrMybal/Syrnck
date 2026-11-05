#include "internal.hpp"
#include <iostream>
#include <sstream>
using namespace syrnck;
namespace {
void check(bool value) {
  require(value, "read lease test failed");
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
void save(const Store &store, const ReadLease &lease, std::uint64_t expiry) {
  auto record = lease.json();
  record["expires_at"] = expiry;
  atomic_write(store.root() / "reads" / (lease.id + ".json"), bytes(record.dump()), true);
}
} // namespace
int main() {
  auto root = fs::temp_directory_path() / path_from_utf8("syrnck-reads-" + hex(random_bytes(16)));
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
        key->fill(0x39);
      }
      auto directory = root / (encrypted ? "encrypted" : "plain");
      auto id = hex(random_bytes(32));
      ObjectKey object;
      std::string fence;
      {
        Store store(directory, key, true);
        check(store.read_leases().empty());
        object = store.put(Kind::chunk, bytes("retained read payload")).first;
        auto lease = store.acquire_read_lease(id, object, 3600);
        fence = lease.fence;
        check(ReadLease::parse(lease.json()).json() == lease.json());
        check(store.acquire_read_lease(id, object, 3600, fence).fence == fence);
        fails([&] { store.acquire_read_lease(id, object, 0); });
        fails([&] { store.acquire_read_lease(id, object, 86401); });
        fails([&] { store.acquire_read_lease(id, object, 3600, std::string(64, '0')); });
        fails([&] { store.acquire_read_lease(id, {Kind::chunk, std::string(64, '0')}, 3600); });
        fails([&] {
          store.acquire_read_lease(hex(random_bytes(32)), {Kind::chunk, std::string(64, '0')}, 3600);
        });
        check(store.gc()["live_objects"] == 1);
        check(store.gc(true)["unreachable_objects"] == 0);
        check(store.prune_read_leases() == 0 && store.contains(object));
        for (auto bad : {Json{{"version", 2}}, Json{{"expires_at", -1}}, Json{{"expires_at", true}},
                         Json{{"state", "published"}}, Json{{"fence", "invalid"}}, Json{{"extra", 1}}}) {
          auto value = lease.json();
          value.update(bad);
          fails([&] { ReadLease::parse(value); });
        }
      }
      {
        Store store(directory, key);
        auto lease = *store.read_lease(id);
        check(lease.fence == fence && store.read_leases().size() == 1);
        check(store.gc(true)["live_objects"] == 1);
        save(store, lease, 1);
        check(store.gc()["unreachable_objects"] == 1);
        check(store.read_lease(id)->state == "active"); // Dry run leaves the journal unchanged.
        check(store.gc(true)["unreachable_objects"] == 1);
        lease = *store.read_lease(id);
        check(lease.state == "expired" && lease.fence != fence);
        save(store, lease, unix_seconds() + 3600); // A wall-clock rollback cannot revive protection.
        fails([&] { store.acquire_read_lease(id, object, 3600, fence); });
        fails([&] { store.acquire_read_lease(id, object, 3600, lease.fence); }); // Source gone.
        store.import(object, bytes("retained read payload"));
        auto acquired = store.acquire_read_lease(id, object, 3600, lease.fence);
        check(acquired.fence != lease.fence);
        fails([&] { store.release_read_lease(id, lease.fence); });
        store.release_read_lease(id, acquired.fence);
        store.release_read_lease(id, acquired.fence);
        fails([&] { store.acquire_read_lease(id, object, 3600); });
        check(store.prune_read_leases() == 1);
        auto recreated = store.acquire_read_lease(id, object, 3600);
        check(recreated.fence != acquired.fence);
        store.release_read_lease(id);
        check(store.prune_read_leases() == 1);
        check(store.gc(true)["unreachable_objects"] == 1);
        fails([&] { store.read_lease("../refs.json"); });
        auto path = directory / "reads" / (id + ".json");
        atomic_write(path, bytes("{"), false);
        fails([&] { store.gc(true); });
        fails([&] { store.prune_read_leases(); });
        fs::remove(path);
        // Persistent foreign records fail closed; confirmed disappearance permits inventory.
        auto foreign = directory / "reads" / "foreign.JSON.tmp";
        atomic_write(foreign, bytes("{"), false);
        fails([&] { store.read_leases(); });
        fails([&] { store.gc(true); });
        fails([&] { store.prune_read_leases(); });
        // A delayed worker cannot be assumed to run within the production 50 ms grace window.
        // Check the vanished-file path directly instead of depending on scheduler timing.
        check(fs::remove(foreign));
        check(transient_metadata_disappeared(foreign));
        check(store.read_leases().empty());
        atomic_write(directory / "reads" / ".tmpinterrupted", bytes("{"), false);
        check(store.read_leases().empty());
        std::stop_source stop;
        stop.request_stop();
        fails([&] { store.read_leases(stop.get_token()); });
      }
    }
    {
      Store source(root / "source", {}, true), partial(root / "partial", {}, true);
      std::istringstream input("verify the entire retained file before granting a lease");
      ObjectKey manifest{Kind::manifest, source.ingest(input).first};
      partial.import(manifest, source.get(manifest));
      auto id = hex(random_bytes(32));
      fails([&] { partial.acquire_read_lease(id, manifest, 3600); });
      check(partial.read_leases().empty());
      auto bad = source.metadata(Kind::manifest, manifest.id);
      bad["file_hash"] = std::string(64, '0');
      auto corrupt = source.put(Kind::manifest, bytes(bad.dump())).first;
      fails([&] { source.acquire_read_lease(id, corrupt, 3600); });
      check(source.read_leases().empty());
      auto lease = source.acquire_read_lease(id, manifest, 3600);
      source.release_read_lease(id, lease.fence);
      auto outside = root / "outside";
      fs::create_directory(outside);
      std::error_code error;
      fs::create_directory_symlink(outside, partial.root() / "reads", error);
      if (!error) {
        fails([&] { partial.read_leases(); });
        fails([&] { partial.gc(true); });
        check(fs::is_empty(outside));
      }
    }
    std::cout << "read leases passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
