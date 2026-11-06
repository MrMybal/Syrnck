#include "native_history_fixture.hpp"
#include "server_access.hpp"
#include <chrono>
#include <thread>
#include <iostream>
using namespace native_test;
using namespace syrnck::net::detail;
namespace {
Json configuration() {
  return Json{
      {"version", 1},
      {"principals", Json::array({Json{{"id", "alice"},
                                       {"token_hash", hash(bytes("test identity"))},
                                       {"role", "writer"},
                                       {"write_paths", Json::array({"Content", "Source"})},
                                       {"branches", Json::array({"main", "scratch"})}}})},
      {"branches",
       Json::array({Json{{"name", "main"}, {"fast_forward", true}, {"lock_paths", Json::array({"Content"})}},
                    Json{{"name", "scratch"}, {"fast_forward", false}, {"lock_paths", Json::array()}}})}};
}
void configurations() {
  const auto good = configuration();
  for (const auto &field : {"version", "principals", "branches"}) {
    auto bad = good;
    bad[field] = nullptr;
    fails([&] { ServerAccess access(bad); });
  }
  for (const auto &field : {"id", "token_hash", "role", "write_paths", "branches"}) {
    auto bad = good;
    bad["principals"][0][field] = nullptr;
    fails([&] { ServerAccess access(bad); });
  }
  for (auto id : {"../bad", "same/path", ""}) {
    auto bad = good;
    bad["principals"][0]["id"] = id;
    fails([&] { ServerAccess access(bad); });
  }
  auto bad = good;
  bad["principals"].push_back(bad["principals"][0]);
  fails([&] { ServerAccess access(bad); });
  bad = good;
  bad["principals"][0]["role"] = "reader";
  fails([&] { ServerAccess access(bad); });
  bad = good;
  bad["principals"][0]["branches"].push_back("unknown");
  fails([&] { ServerAccess access(bad); });
  bad = good;
  bad["principals"][0]["write_paths"].push_back("content");
  fails([&] { ServerAccess access(bad); });
  bad = good;
  bad["branches"].push_back(bad["branches"][0]);
  fails([&] { ServerAccess access(bad); });
  ServerAccess access(good);
  require(!access.authenticate("wrong"), "wrong principal token authenticated");
  require(access.authenticate("test identity")->id == "alice", "principal token was not resolved");
  require(access.visible("main") && !access.visible("heads/main"), "internal references were exposed");
  fails([&] { decode_lock_claims(Json::array({Json{{"path", "a"}, {"fence", 0}}})); });
  fails([&] {
    decode_lock_claims(Json::array({Json{{"path", "a"}, {"fence", 1}}, Json{{"path", "A"}, {"fence", 1}}}));
  });
}
void policy(const fs::path &path, const std::optional<Key> &key, unsigned layout) {
  Fixture f(path, key);
  const auto original = f.file("first"), next = f.file("second"), source = f.file("code");
  const auto a = f.revision(Json{{"Content/map", original}});
  const auto b = f.revision(Json{{"Content/map", next}}, Json::array({a}));
  const auto forbidden =
      f.revision(Json{{"Content/map", next}, {"Contentious/private", source}}, Json::array({b}));
  const auto hidden = f.revision(Json{{"Content/map", next}}, Json::array({forbidden}));
  f.pack(layout);
  ServerAccess access(configuration());
  const auto &user = *access.authenticate("test identity");
  AssetLocks locks(f.store);
  fails([&] { access.publication(f.store, user, "main", {Kind::revision, a}, {}, {}); });
  const auto held = locks.acquire("alice", {"Content/map"});
  const auto fence = held["locks"][0]["fence"].get<std::uint64_t>();
  const std::vector<AssetLockClaim> claims{{"content/MAP", fence}};
  const auto before = f.physical();
  const auto check = access.publication(f.store, user, "main", {Kind::revision, a}, {}, claims);
  require(f.physical() == before, "server policy planning wrote storage");
  require(f.store.compare_update_refs({{"main", {}, ObjectKey{Kind::revision, a}}}, {{}, check}),
          "server policy refused an authorized publication");
  fails([&] {
    access.publication(f.store, user, "main", {Kind::revision, hidden}, ObjectKey{Kind::revision, a}, claims);
  });
  fails([&] {
    access.publication(f.store, user, "main", {Kind::manifest, original["manifest"].get<std::string>()}, {},
                       claims);
  });
  fails([&] {
    access.publication(f.store, user, "unknown", {Kind::revision, b}, ObjectKey{Kind::revision, a}, claims);
  });
  access.publication(f.store, user, "main", {Kind::revision, b}, ObjectKey{Kind::revision, a}, claims);
  fails([&] {
    access.publication(f.store, user, "main", {Kind::revision, a}, ObjectKey{Kind::revision, b}, claims);
  });
  access.publication(f.store, user, "scratch", {Kind::revision, a}, ObjectKey{Kind::revision, b}, claims);
  locks.release("alice", claims);
  locks.acquire("alice", {"Content/map"});
  fails([&] {
    access.publication(f.store, user, "main", {Kind::revision, b}, ObjectKey{Kind::revision, a}, claims);
  });
  // Even an unconfigured lock prefix honors an existing exact asset lease.
  locks.acquire("bob", {"Source/code"});
  const auto c = f.revision(Json{{"Content/map", original}, {"Source/code", source}}, Json::array({a}));
  fails([&] {
    access.publication(f.store, user, "scratch", {Kind::revision, c}, ObjectKey{Kind::revision, a}, {});
  });
}
void final_expiry(const fs::path &path) {
  Fixture f(path);
  const auto a = f.revision(Json{{"Content/map", f.file("asset")}});
  AssetLocks locks(f.store);
  const auto held = locks.acquire("alice", {"Content/map"}, 2);
  const auto deadline = held["locks"][0]["expires_at"].get<std::uint64_t>();
  ServerAccess access(configuration());
  const auto &user = *access.authenticate("test identity");
  const auto check = access.publication(f.store, user, "main", {Kind::revision, a}, {},
                                        {{"Content/map", held["locks"][0]["fence"].get<std::uint64_t>()}});
  const auto before = f.store.refs();
  fails([&] {
    f.store.compare_update_refs({{"main", {}, ObjectKey{Kind::revision, a}}},
                                {{}, [&](const ProgressEvent &event) {
                                   if (event.phase == ProgressPhase::publishing)
                                     while (unix_seconds() < deadline)
                                       std::this_thread::sleep_for(std::chrono::milliseconds(10));
                                   check(event);
                                 }});
  });
  require(f.store.refs() == before, "expired asset lock published after closure validation");
}
} // namespace
int main() {
  Temporary temporary;
  try {
    configurations();
    for (bool encrypted : {false, true})
      for (unsigned layout = 0; layout < 3; ++layout) {
        std::optional<Key> key;
        if (encrypted) {
          key.emplace();
          key->fill(77);
        }
        policy(temporary.path / (std::to_string(encrypted) + '-' + std::to_string(layout)), key, layout);
      }
    final_expiry(temporary.path / "expiry");
    std::cout << "Strict server identities, write ACLs, history checks and publication-time asset fencing\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
