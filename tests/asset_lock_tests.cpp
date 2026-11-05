#include "native_history_fixture.hpp"
#include <syrnck/asset_locks.hpp>
#include <iostream>
using namespace native_test;
namespace {
std::uint64_t fence(const Json &result) {
  return result["locks"][0]["fence"].get<std::uint64_t>();
}
void lifecycle(const fs::path &root, std::optional<Key> key, unsigned layout) {
  Fixture f(root, key);
  std::uint64_t now = 1000;
  AssetLocks locks(f.store, [&] { return now; });
  const auto before = f.physical();
  require(locks.list()["locks"].empty(), "new store has asset locks");
  const auto plan = locks.acquire_plan("alice", {"Content/Map.uasset", "Content/Map.ubulk"}, 30);
  require(plan["ready"] == true && plan["locks"].size() == 2 && f.physical() == before,
          "asset lock plan wrote objects");
  const auto acquired = locks.acquire("alice", {"Content/Map.uasset", "Content/Map.ubulk"}, 30);
  const auto first = fence(acquired);
  require(first == 1 && acquired["locks"] == plan["locks"], "asset lock acquisition differs from preview");
  f.pack(layout);
  const auto unchanged = f.physical();
  const auto blocked = locks.acquire_plan("bob", {"content/map.UASSET", "Content/Other.uasset"}, 30);
  require(blocked["ready"] == false && blocked["conflicts"].size() == 1,
          "portable alias bypassed an asset lock");
  fails([&] { locks.acquire("bob", {"content/map.UASSET", "Content/Other.uasset"}, 30); });
  fails([&] { locks.acquire("alice", {"Content/Map.uasset"}); });
  fails([&] { locks.release("bob", {{"Content/Map.uasset", first}}); });
  fails([&] { locks.release("alice", {{"Content/Map.uasset", first}, {"Content/Map.ubulk", first + 1}}); });
  fails([&] { locks.acquire("alice", {"file", "FILE"}); });
  require(f.physical() == unchanged, "rejected asset lock batch partially published");
  now = 1005;
  const auto renewed =
      locks.renew("alice", {{"Content/Map.uasset", first}, {"Content/Map.ubulk", first}}, 60);
  for (const auto &row : renewed["locks"])
    require(row["fence"] == first && row["created_at"] == 1000 && row["expires_at"] == 1065,
            "renewal lost acquisition identity or changed its fence");
  auto second = locks.acquire("bob", {"Content/Other.uasset"}, 10);
  require(fence(second) > first, "global fencing generation was reused");
  const auto page = locks.list({}, 1);
  require(page["locks"].size() == 1 && page["next"].is_string(), "asset lock pagination lost its cursor");
  const auto next = locks.list(page["next"].get<std::string>(), 10);
  require(next["locks"].size() == 2 && next["next"].is_null(),
          "asset lock pagination repeated or skipped entries");
  now = 1065;
  fails([&] { locks.renew("alice", {{"Content/Map.uasset", first}}, 30); });
  const auto preview = f.physical();
  require(locks.prune()["locks"].size() == 3 && f.physical() == preview, "lock pruning preview wrote data");
  // Expired ownership can be removed, but cannot renew or publish as active.
  locks.release("alice", {{"Content/Map.uasset", first}});
  require(locks.prune(true)["locks"].size() == 2 && locks.list()["locks"].empty(),
          "expired lock pruning lost an entry");
  f.store.gc(true);
  const auto replacement = locks.acquire("bob", {"Content/Map.uasset"}, 20);
  require(fence(replacement) > fence(second), "prune/GC reset the fencing counter");
  const auto current = f.physical();
  fails([&] { locks.release("alice", {{"Content/Map.uasset", first}}); });
  fails([&] { locks.renew("bob", {{"Content/Map.uasset", first}}, 30); });
  require(f.physical() == current && locks.list()["locks"][0]["active"] == true,
          "stale client changed a replacement lock");
  locks.release("bob", {{"Content/Map.uasset", fence(replacement)}});
  f.store.gc(true);
  require(f.store.fsck() > 0, "asset locks damaged the object store");
}
void cancellation(const fs::path &root) {
  Fixture f(root);
  std::uint64_t now = 1000;
  AssetLocks locks(f.store, [&] { return now; });
  const auto before = f.physical();
  std::stop_source stop;
  stop.request_stop();
  fails([&] { locks.list({}, 100, {stop.get_token()}); });
  fails([&] { locks.acquire("alice", {"a"}, 30, {stop.get_token()}); });
  require(f.physical() == before, "pre-cancelled lock operation wrote data");
  const auto refs = f.store.refs();
  fails([&] {
    locks.acquire("alice", {"a", "b"}, 10, {{}, [&](const ProgressEvent &event) {
                                              if (event.phase == ProgressPhase::publishing)
                                                now = 1010;
                                            }});
  });
  require(f.store.refs() == refs, "expired-before-publication lock became visible");
  now = 1000;
  fails([&] {
    locks.acquire("alice", {"a", "b"}, 10, {{}, [&](const ProgressEvent &event) {
                                              if (event.phase == ProgressPhase::publishing)
                                                now = 999;
                                            }});
  });
  require(f.store.refs() == refs, "backwards clock published an invalid acquisition");
  now = 2000;
  const auto acquired = locks.acquire("alice", {"a", "b"}, 10);
  const auto active = f.store.refs();
  fails([&] {
    locks.renew("alice", {{"a", fence(acquired)}, {"b", fence(acquired)}}, 30,
                {{}, [&](const ProgressEvent &event) {
                   if (event.phase == ProgressPhase::publishing)
                     now = 2010;
                 }});
  });
  require(f.store.refs() == active, "expired lease renewal partially published");
  now = 2005;
  std::stop_source publishing;
  fails([&] {
    locks.release("alice", {{"a", fence(acquired)}, {"b", fence(acquired)}},
                  {publishing.get_token(), [&](const ProgressEvent &event) {
                     if (event.phase == ProgressPhase::publishing)
                       publishing.request_stop();
                   }});
  });
  require(f.store.refs() == active, "cancelled release partially published");
  locks.release("alice", {{"a", fence(acquired)}, {"b", fence(acquired)}});
}
void invalid(const fs::path &root) {
  Fixture f(root);
  AssetLocks locks(f.store, [] { return 1000; });
  const auto before = f.physical();
  for (auto owner : {"", "bad\nowner"})
    fails([&] { locks.acquire(owner, {"a"}); });
  fails([&] { locks.acquire(std::string(257, 'a'), {"a"}); });
  fails([&] { locks.acquire("alice", {}); });
  fails([&] { locks.acquire("alice", {"../bad"}); });
  fails([&] { locks.acquire("alice", {"a"}, 0); });
  fails([&] { locks.acquire("alice", {"a"}, 86401); });
  fails([&] { locks.acquire("alice", {std::string(4097, 'a')}); });
  fails([&] { locks.acquire("\xc2\x85", {"a"}); });
  fails([&] { locks.list({}, 0); });
  fails([&] { locks.list({}, 1001); });
  require(f.physical() == before, "invalid lock arguments wrote objects");
}
void corruption(const fs::path &root) {
  Fixture f(root);
  AssetLocks locks(f.store, [] { return 1000; });
  locks.acquire("alice", {"a"}, 30);
  const auto refs = f.store.refs();
  const auto name = "asset-locks/paths/" + hash(bytes(portable_fold("a")));
  const auto original = refs.at(name), counter = refs.at("asset-locks/clock");
  const auto record = parse_json(f.store.get(original));
  for (const auto &field : {"version", "type", "fence", "created_at", "expires_at", "owner", "path"}) {
    auto bad = record;
    bad[field] = Json::array();
    f.store.set_ref(name, f.store.put(Kind::chunk, bytes(bad.dump())).first);
    fails([&] { locks.list(); });
    fails([&] { locks.acquire("bob", {"b"}); });
  }
  f.store.set_ref(name, original);
  for (const auto &generation : {Json(0), Json(-1), Json("invalid")}) {
    const auto bad = Json{{"version", 1}, {"type", "asset-lock-counter"}, {"generation", generation}};
    f.store.set_ref("asset-locks/clock", f.store.put(Kind::chunk, bytes(bad.dump())).first);
    fails([&] { locks.list(); });
  }
  const auto maximum = Json{{"version", 1}, {"type", "asset-lock-counter"}, {"generation", UINT64_MAX}};
  f.store.set_ref("asset-locks/clock", f.store.put(Kind::chunk, bytes(maximum.dump())).first);
  fails([&] { locks.acquire("bob", {"b"}); });
  f.store.remove_ref("asset-locks/clock");
  fails([&] { locks.list(); });
  f.store.set_ref("asset-locks/clock", counter);
  require(locks.list()["locks"].size() == 1,
          "lock audit did not recover from repaired administrative metadata");
}
} // namespace
int main() {
  Temporary temp;
  try {
    for (bool encrypted : {false, true})
      for (unsigned layout = 0; layout < 3; ++layout) {
        std::optional<Key> key;
        if (encrypted) {
          key.emplace();
          key->fill(58);
        }
        lifecycle(temp.path / (std::to_string(encrypted) + '-' + std::to_string(layout)), key, layout);
      }
    cancellation(temp.path / "cancel");
    invalid(temp.path / "invalid");
    corruption(temp.path / "corrupt");
    std::cout << "Atomic asset lease batches, portable identity, expiration, fencing, pruning and GC\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
