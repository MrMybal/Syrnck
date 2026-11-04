#include "internal.hpp"
#include <fstream>
#include <iostream>
#include <sstream>
#include <source_location>
#include <thread>
#include <chrono>
using namespace syrnck;
namespace {
void check(bool value, std::source_location location = std::source_location::current()) {
  require(value, "transfer retention test failed at line " + std::to_string(location.line()));
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
TransferSession session(const ObjectKey &root, std::string direction, std::string reference = "asset") {
  TransferSession value;
  value.direction = direction;
  value.reference = reference;
  value.root = root;
  value.endpoint = hash(bytes("retention endpoint"));
  value.id = TransferSession::identity(direction, value.endpoint, root, reference);
  return value;
}
ObjectKey file(Store &store, std::string_view content) {
  std::istringstream input{std::string(content)};
  return {Kind::manifest, store.ingest(input, Chunking::fixed_size(4)).first};
}
fs::path loose(const Store &store, const ObjectKey &key) {
  return store.root() / "objects" / kind_name(key.kind) / key.id.substr(0, 2) / key.id;
}
} // namespace
int main() {
  auto root = fs::temp_directory_path() / path_from_utf8("syrnck-retention-" + hex(random_bytes(16)));
  fs::create_directory(root);
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code ec;
      fs::remove_all(path, ec);
    }
  } cleanup{root};
  std::string context = "source setup";
  try {
    Store source(root / "source", {}, true);
    auto manifest = file(source, "aaaabbbbccccdddd");
    auto dependencies = syrnck::dependencies(manifest.kind, source.get(manifest));
    check(dependencies.size() == 4);
    for (bool encrypted : {false, true})
      for (bool packed : {false, true}) {
        std::optional<Key> secret;
        if (encrypted) {
          secret.emplace();
          secret->fill(0x39);
        }
        auto name = std::string(encrypted ? "encrypted-" : "plain-") + (packed ? "packed" : "loose");
        auto directory = root / name;
        context = name + " partial transfer";
        auto partial = session(manifest, "pull");
        auto orphan = ObjectKey{};
        {
          Store target(directory, secret, true);
          target.import(dependencies[0], source.get(dependencies[0]));
          target.import(dependencies[2], source.get(dependencies[2])); // A gap must not hide later blocks.
          orphan = target.put(Kind::chunk, bytes("unrelated orphan")).first;
          if (packed)
            target.pack(); // Pack verified chunks before importing incomplete metadata.
          target.import(manifest, source.get(manifest));
          target.write_transfer_session(partial);
          auto before = read_bytes(directory / "transfers" / (partial.id + ".json"));
          auto preview = target.gc();
          check(preview["live_objects"] == 3 && preview["unreachable_objects"] == 1);
          check(target.contains(orphan));
          std::stop_source stop;
          fails([&] {
            target.gc(true, {stop.get_token(), [&](const ProgressEvent &e) {
                               if (e.phase == ProgressPhase::publishing)
                                 stop.request_stop();
                             }});
          });
          check(target.contains(orphan));
          auto result = target.gc(true);
          check(result["live_objects"] == 3 && result["unreachable_objects"] == 1);
          check(!target.contains(orphan) && target.refs().empty());
          check(read_bytes(directory / "transfers" / (partial.id + ".json")) == before);
        }
        {
          context = name + " complete transfer";
          Store target(directory, secret);
          check(target.get(dependencies[0]) == source.get(dependencies[0]));
          check(target.get(dependencies[2]) == source.get(dependencies[2]));
          for (auto index : {1, 3})
            target.import(dependencies[index], source.get(dependencies[index]));
          // Completion validates the whole file; corruption cannot be hidden by partial traversal.
          auto bad = source.metadata(Kind::manifest, manifest.id);
          bad["file_hash"] = std::string(64, '0');
          auto invalid = target.put(Kind::manifest, bytes(bad.dump())).first;
          auto held_bad = session(invalid, "pull", "invalid");
          target.write_transfer_session(held_bad);
          auto garbage = target.put(Kind::chunk, bytes("keep on failure")).first;
          fails([&] { target.gc(true); });
          check(target.contains(garbage));
          target.remove_transfer_session(held_bad.id);
          target.set_ref("received", manifest);
          partial.state = "complete";
          target.write_transfer_session(partial);
          target.gc(true);
          check(!target.contains(garbage));
          check(target.fsck() == 5);
          std::ostringstream output;
          target.reconstruct(manifest.id, output);
          check(output.str() == "aaaabbbbccccdddd");
        }
        // Full push and read roots retain their closure while unrelated objects are collectable.
        {
          context = name + " push/read retention";
          Store target(root / (name + "-complete"), secret, true);
          for (const auto &key : source.closure({manifest}, true))
            target.import(key, source.get(key));
          auto push = session(manifest, "push");
          target.write_transfer_session(push);
          auto lease = target.acquire_read_lease(hex(random_bytes(32)), manifest, 3600);
          auto garbage = target.put(Kind::chunk, bytes("unrelated complete-root garbage")).first;
          if (packed)
            target.pack();
          context = name + " push/read retention initial GC";
          check(target.gc(true)["unreachable_objects"] == 1 && !target.contains(garbage));
          target.remove_transfer_session(push.id);
          context = name + " read-only retention GC";
          check(target.gc(true)["live_objects"] == 5);
          target.release_read_lease(lease.id, lease.fence);
          context = name + " released read retention GC";
          check(target.gc(true)["unreachable_objects"] == 5);
        }
      }
    {
      context = "revision retention";
      Json revision{{"version", 1},
                    {"parents", Json::array()},
                    {"message", "parent"},
                    {"timestamp", std::uint64_t{1}},
                    {"files", Json::object()}};
      auto parent = source.put(Kind::revision, bytes(revision.dump())).first;
      revision["parents"].push_back(parent.id);
      revision["files"]["asset.bin"] = Json{{"manifest", manifest.id}, {"executable", false}};
      auto head = source.put(Kind::revision, bytes(revision.dump())).first;
      Store target(root / "revision", {}, true);
      target.import(head, source.get(head));
      target.import(manifest, source.get(manifest));
      target.import(dependencies[2], source.get(dependencies[2]));
      auto partial = session(head, "pull");
      target.write_transfer_session(partial);
      target.put(Kind::chunk, bytes("unrelated revision garbage"));
      auto result = target.gc(true);
      check(result["live_objects"] == 3 && result["unreachable_objects"] == 1);
      for (const auto &key : source.closure({head}, true))
        target.import(key, source.get(key));
      target.set_ref("received", head);
      check(target.gc(true)["live_objects"] == 7 && target.contains(parent));
      target.export_revision(head.id, root / "revision-output");
      check(text(read_bytes(root / "revision-output" / "asset.bin")) == "aaaabbbbccccdddd");
    }
    {
      context = "missing/corrupt roots";
      Store target(root / "missing", {}, true);
      ObjectKey missing{Kind::manifest, std::string(64, '0')};
      auto push = session(missing, "push");
      target.write_transfer_session(push);
      auto garbage = target.put(Kind::chunk, bytes("preserve when full root is missing")).first;
      fails([&] { target.gc(true); });
      check(target.contains(garbage));
      target.remove_transfer_session(push.id);
      auto pull = session(missing, "pull");
      target.write_transfer_session(pull);
      check(target.gc(true)["unreachable_objects"] == 1);
      // Existing corrupt bytes are never treated as an absent partial dependency.
      target.import(manifest, source.get(manifest));
      target.import(dependencies[2], source.get(dependencies[2]));
      pull = session(manifest, "pull", "corrupt");
      target.write_transfer_session(pull);
      atomic_write(loose(target, dependencies[2]), bytes("broken envelope"), true);
      garbage = target.put(Kind::chunk, bytes("preserve when partial data is corrupt")).first;
      fails([&] { target.gc(true); });
      check(target.contains(garbage));
    }
    {
      context = "receipt pruning";
      Store target(root / "prune", {}, true);
      auto payload = target.put(Kind::chunk, bytes("payload untouched by receipt pruning")).first;
      target.set_ref("retained", payload);
      std::vector<TransferSession> receipts;
      for (auto state : {"transferring", "publishing", "uncertain", "conflict", "complete", "complete"}) {
        auto record = session(payload, "push", "record-" + std::to_string(receipts.size()));
        record.state = state;
        target.write_transfer_session(record);
        receipts.push_back(record);
      }
      auto refs = read_bytes(target.root() / "refs.json");
      auto preview = target.prune_transfer_sessions();
      check(preview["completed"] == 2 && preview["unfinished"] == 4 && preview["removed"] == 0);
      auto malformed = session(payload, "pull", "malformed");
      auto path = target.root() / "transfers" / (malformed.id + ".json");
      atomic_write(path, bytes("{"), false);
      fails([&] { target.prune_transfer_sessions(true); });
      for (const auto &record : receipts)
        check(target.transfer_session(record.id).has_value());
      target.remove_transfer_session(malformed.id);
      std::stop_source stop;
      fails([&] {
        target.prune_transfer_sessions(true, {stop.get_token(), [&](const ProgressEvent &e) {
                                                if (e.phase == ProgressPhase::publishing && e.bytes > 0)
                                                  stop.request_stop();
                                              }});
      });
      auto remaining = target.transfer_sessions();
      check(remaining.size() == 5);
      check(target.prune_transfer_sessions(true)["removed"] == 1);
      check(target.transfer_sessions().size() == 4);
      for (std::size_t i = 0; i < 4; ++i)
        check(target.transfer_session(receipts[i].id)->json() == receipts[i].json());
      check(read_bytes(target.root() / "refs.json") == refs &&
            text(target.get(payload)) == "payload untouched by receipt pruning");
      check(target.prune_transfer_sessions(true)["removed"] == 0);
    }
    {
      context = "foreign temporary object inventory";
      Store target(root / "foreign-inventory", {}, true);
      auto retained = target.put(Kind::chunk, bytes("retained during foreign temporary cleanup")).first;
      target.set_ref("retained", retained);
      auto garbage = target.put(Kind::chunk, bytes("collect unrelated temporary inventory garbage")).first;
      auto foreign = loose(target, retained).parent_path() / (std::string(64, 'A') + ".tmp");
      atomic_write(foreign, bytes("persistent foreign temporary entry"), false);
      fails([&] { target.gc(true); });
      check(target.contains(retained) && target.contains(garbage) && fs::exists(foreign));
      std::jthread disappearance([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::error_code error;
        fs::remove(foreign, error);
      });
      check(target.gc(true)["unreachable_objects"] == 1);
      disappearance.join();
      check(target.contains(retained) && !target.contains(garbage) && !fs::exists(foreign));
      check(target.fsck() == 1);
    }
    std::cout << "Selective transfer retention and receipt pruning passed\n";
  } catch (const std::exception &e) {
    std::cerr << context << ": " << e.what() << '\n';
    std::error_code error;
    for (fs::recursive_directory_iterator it(root, error), end; !error && it != end; it.increment(error))
      std::cerr << utf8(it->path().lexically_relative(root)) << '\n';
    return 1;
  }
}
