#include "internal.hpp"
#include <fstream>
#include <iostream>
#include <sstream>
using namespace syrnck;
namespace {
void check(bool value) {
  require(value, "transfer session test failed");
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
TransferSession session(const ObjectKey &root, std::string direction = "push") {
  TransferSession result;
  result.direction = direction;
  result.endpoint = hash(bytes("canonical endpoint"));
  result.reference = "asset";
  result.root = root;
  result.id = TransferSession::identity(result.direction, result.endpoint, root, result.reference);
  return result;
}
} // namespace
int main() {
  auto root = fs::temp_directory_path() / path_from_utf8("syrnck-sessions-" + hex(random_bytes(16)));
  fs::create_directory(root);
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code error;
      fs::remove_all(path, error);
    }
  } cleanup{root};
  try {
    for (bool encrypted : {false, true}) {
      std::optional<Key> key;
      if (encrypted) {
        key.emplace();
        key->fill(0x73);
      }
      auto directory = root / (encrypted ? "encrypted" : "plain");
      std::string id;
      ObjectKey object;
      {
        Store store(directory, key, true);
        check(store.transfer_sessions().empty() && !fs::exists(directory / "transfers"));
        object = store.put(Kind::chunk, bytes("retained transfer payload")).first;
        auto record = session(object);
        id = record.id;
        auto document = record.json();
        check(TransferSession::parse(document).json() == document);
        for (auto field :
             {"id", "direction", "endpoint", "reference", "state", "root", "expected", "version"}) {
          auto bad = document;
          bad.erase(field);
          fails([&] { (void)TransferSession::parse(bad); });
        }
        auto legacy = document;
        legacy.erase("read_lease");
        legacy["version"] = 2;
        check(TransferSession::parse(legacy).json() == document);
        legacy.erase("upload_lease");
        legacy["version"] = 1;
        check(TransferSession::parse(legacy).json() == document);
        for (auto bad :
             {Json{{"version", true}}, Json{{"version", 4}}, Json{{"direction", "other"}},
              Json{{"state", "other"}}, Json{{"id", std::string(64, '0')}}, Json{{"expected", Json::array()}},
              Json{{"extra", 1}}, Json{{"read_lease", std::string(64, 'a')}}}) {
          auto changed = document;
          changed.update(bad);
          fails([&] { (void)TransferSession::parse(changed); });
        }
        store.write_transfer_session(record);
        auto path = directory / "transfers" / (id + ".json");
        auto before = read_bytes(path);
        std::stop_source stopped;
        stopped.request_stop();
        record.state = "complete";
        fails([&] { store.write_transfer_session(record, stopped.get_token()); });
        check(read_bytes(path) == before);
        record.state = "invalid";
        fails([&] { store.write_transfer_session(record); });
        check(read_bytes(path) == before);
        // Uncommitted atomic stages must not release the canonical GC hold.
        atomic_write(directory / "transfers" / ".tmpinterrupted", bytes("truncated"), false);
      }
      {
        Store store(directory, key);
        auto record = *store.transfer_session(id);
        check(record.state == "transferring" && store.transfer_sessions().size() == 1);
        for (std::string state : {"transferring", "publishing", "uncertain", "conflict"}) {
          record.state = state;
          store.write_transfer_session(record);
          fails([&] { (void)store.gc(); });
          fails([&] { (void)store.gc(true); });
          check(text(store.get(object)) == "retained transfer payload");
        }
        record.state = "complete";
        store.write_transfer_session(record);
        check(store.gc(true)["unreachable_objects"] == 1 && store.fsck() == 0);
        store.remove_transfer_session(id);
        check(store.transfer_sessions().empty());
        fails([&] { store.remove_transfer_session(id); });
        fails([&] { (void)store.transfer_session("../refs.json"); });
        // Bad records fail closed, but explicit discard can remove them.
        auto path = directory / "transfers" / (id + ".json");
        atomic_write(path, bytes("{"), false);
        fails([&] { (void)store.transfer_sessions(); });
        fails([&] { (void)store.gc(true); });
        store.remove_transfer_session(id);
        atomic_write(path, bytes(std::string(16385, 'x')), false);
        fails([&] { (void)store.transfer_session(id); });
        store.remove_transfer_session(id);
      }
    }
    {
      Store source(root / "source", {}, true), partial(root / "partial", {}, true);
      std::istringstream data("a file whose chunks are missing from the destination");
      auto id = source.ingest(data).first;
      ObjectKey manifest{Kind::manifest, id};
      partial.import(manifest, source.get(manifest));
      auto record = session(manifest, "pull");
      record.read_lease = hex(random_bytes(32));
      check(TransferSession::parse(record.json()).read_lease == record.read_lease);
      partial.write_transfer_session(record);
      fails([&] { partial.fsck(); });
      fails([&] { partial.gc(true); });
      partial.remove_transfer_session(record.id);
      check(partial.gc(true)["unreachable_objects"] == 1 && partial.fsck() == 0);
      auto outside = root / "outside";
      fs::create_directory(outside);
      fs::remove_all(partial.root() / "transfers");
      std::error_code error;
      fs::create_directory_symlink(outside, partial.root() / "transfers", error);
      if (!error) {
        fails([&] { partial.write_transfer_session(record); });
        fails([&] { partial.gc(true); });
        check(fs::is_empty(outside));
      }
    }
    std::cout << "Session schema, reopen, atomic rejection, GC holds, discard and path isolation passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
