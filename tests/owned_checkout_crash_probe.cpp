#include "native_history_fixture.hpp"
#include "native_context.hpp"
#include <syrnck/native_sync.hpp>
#include <iostream>
using namespace native_test;
namespace {
bool tracing = false, armed = false;
std::string phase, kind;
std::size_t occurrence = 0, seen = 0;
Json events = Json::array();
std::map<std::pair<std::string, std::string>, std::size_t> counts;
fs::path fixture_root;
std::string server_base = "http://127.0.0.1:9";
std::optional<Key> encryption(const Json &info) {
  if (!info["encrypted"].get<bool>()) return {};
  Key key; key.fill(92); return key;
}
SigningKey signing(unsigned value) { Key seed; seed.fill(static_cast<std::uint8_t>(value)); return SigningKey::from_seed(seed); }
net::Remote endpoint(bool different = false) {
  return net::Remote(different ? "http://127.0.0.1:10" : server_base, "syrnck-owned-recovery-test-token-0000");
}
fs::path working(const fs::path &root, const Json &info) { return root / (info["scope"] == "primary" ? "primary" : "edit"); }
std::string service(const Store &store) {
  return hash(bytes(Json{{"version", 1}, {"type", "native-push-sync"}, {"workspace", store.native_workspace_name()},
      {"target", store.native_target()}, {"remote_branch", "main"}, {"endpoint", endpoint().endpoint_fingerprint()}}.dump()));
}
void prepare(const fs::path &root, bool encrypted, const std::string &scope, const std::string &action, bool transition) {
  Fixture f(root / "primary", encryption(Json{{"encrypted", encrypted}}));
  auto files = Json{{".syrnckignore", f.file("*.keep\n")}, {"Content/asset", f.file("hidden before")}, {"Source/item", f.file("before")}};
  const auto base = f.revision(files);
  auto incoming = files;
  incoming["Content/asset"] = f.file("hidden after");
  if (transition) incoming.erase("Source/item");
  incoming[transition ? "Source/item/leaf" : "Source/item"] = f.file("after");
  auto source = f.revision(incoming, Json::array({base}));
  f.store.use_native_identity({"Alice", "alice@example.invalid", 0}, signing(95));
  auto signed_source = f.store.metadata(Kind::revision, source); signed_source["timestamp"] = 42;
  source = f.store.put(Kind::revision, bytes(f.store.native_commit_metadata(std::move(signed_source)).dump())).first.id;
  std::string old = base;
  if (action == "merge") { files["Source/own"] = f.file("independent local work"); old = f.revision(files, Json::array({base})); }
  if (action == "conflict-merge") { files["Source/item"] = f.file("own conflicting bytes"); old = f.revision(files, Json::array({base})); }
  f.install(old); f.store.set_ref("incoming", {Kind::revision, source});
  atomic_write(root / "fixture.json", bytes(Json{{"encrypted", encrypted}, {"scope", scope}, {"action", action},
      {"transition", transition}, {"old", old}, {"source", source}}.dump()), false);
}
void bind(const fs::path &root, Json info) {
  Store store(root / "primary/.syrnck", encryption(info));
  const auto directory = working(root, info);
  const auto old = info["old"].get<std::string>();
  if (info["scope"] != "primary") {
    if (info["scope"] == "partial") store.register_partial_native_workspace(root / "primary", "edit", directory, {"Source"});
    else store.register_native_workspace(root / "primary", "edit", directory);
    if (info["scope"] != "detached") store.create_native_branch("edit", old);
    store.use_native_workspace("edit");
    if (info["scope"] == "detached") store.detach_checkout(directory, old);
    else store.switch_directory(directory, "edit");
  }
  atomic_write(directory / "unrelated.keep", bytes("ignored local bytes"), false);
  info["owner"] = service(store);
  store.pack(); atomic_write(root / "fixture.json", bytes(info.dump()), true);
}
void select(Store &store, const Json &info) { store.use_native_workspace(info["scope"] == "primary" ? "" : "edit"); }
bool perform(Store &store, const fs::path &root, const Json &info) {
  select(store, info);
  const auto directory = working(root, info);
  const auto branch = store.native_target();
  const auto pending = store.checkout_status();
  if (pending["pending"].get<bool>()) {
    require(store.native_identity().is_null() && pending["owner"] == info["owner"], "recovery imported a private key or wrong owner");
    net::NativeSync sync(store, directory, {"main", false, true, true});
    const auto result = sync.tick(store, endpoint());
    require(result["state"] == "recovered" && result["checkout"]["target"] == pending["target"], "agent did not resume its frozen owned target offline");
    return true;
  }
  if (store.native_head()["revision"] != info["old"]) return false;
  store.require_features({"resident-sync-v1"});
  const auto observation = store.native_workspace_reference("sync/push/" + service(store) + "/observed");
  if (!store.refs().contains(observation)) store.set_ref(observation, {Kind::revision, info["source"].get<std::string>()});
  if (info["action"] == "checkout")
    store.checkout_directory_owned(directory, branch, info["source"].get<std::string>(), service(store));
  else {
    store.use_native_identity({"Bob", "bob@example.invalid", 60}, signing(96));
    NativeMergeOptions options; options.checkout_owner = service(store);
    options.conflict_copies = info["action"] == "conflict-merge";
    store.merge_directory(directory, branch, info["old"].get<std::string>(), info["source"].get<std::string>(), "owned signed merge", options);
  }
  return false;
}
void refuse(Store &store, const fs::path &root, const Json &info) {
  require(store.native_identity().is_null(), "refusal loaded a signing key");
  if (!store.checkout_status()["pending"].get<bool>()) return;
  net::NativeSync sync(store, working(root, info), {"main", false, true, true});
  require(sync.tick(store, endpoint(true))["reason"] == "checkout_pending", "another endpoint took over an owned checkout");
  net::NativeSync publisher(store, working(root, info), {"main", false});
  require(publisher.tick(store, endpoint())["reason"] == "checkout_pending", "publication-only agent resumed an incoming checkout");
}
Json verify(Store &store, const fs::path &root, const Json &info, bool complete) {
  select(store, info);
  store.fsck();
  const auto pending = store.checkout_status();
  const auto head = store.native_head()["revision"].get<std::string>();
  (void)store.closure({{Kind::revision, info["old"].get<std::string>()}, {Kind::revision, info["source"].get<std::string>()}, {Kind::revision, head}}, true);
  const auto directory = working(root, info);
  require(text(read_bytes(directory / "unrelated.keep")) == "ignored local bytes", "recovery changed unrelated local work");
  if (pending["pending"].get<bool>()) {
    require(pending["owner"] == info["owner"] && pending["version"] == 11, "pending checkout lost its owner");
    refuse(store, root, info);
  }
  if (head != info["old"].get<std::string>()) {
    const auto revision = store.metadata(Kind::revision, head);
    require(revision["version"] == 2 && revision["signature"].is_object(), "owned recovery lost the prepared signature");
    if (info["action"] == "checkout") require(head == info["source"].get<std::string>(), "recovery retargeted an incoming revision");
    else require(revision["parents"] == Json::array({info["old"], info["source"]}) && revision["committer"]["name"] == "Bob", "recovery changed merge parents or signer");
  }
  if (complete) {
    require(!pending["pending"].get<bool>() && head != info["old"].get<std::string>(), "owned checkout did not finish");
    require(store.workspace_status(directory, store.native_target())["clean"] == true, "owned recovery left mismatched working files");
    if (info["action"] == "conflict-merge") {
      const bool keep_current = info["old"].get<std::string>() < info["source"].get<std::string>();
      require(text(read_bytes(directory / "Source/item")) == (keep_current ? "own conflicting bytes" : "after"),
          "owned recovery changed the deterministic conflict winner");
      std::size_t copies = 0;
      const auto candidate = store.metadata(Kind::revision, head);
      for (const auto &[path, entry] : candidate["files"].items()) {
        (void)entry;
        if (path.find(".syrnck-conflict-") != path.npos) {
          ++copies;
          require(text(read_bytes(directory / path_from_utf8(path))) == (keep_current ? "after" : "own conflicting bytes"),
              "owned recovery discarded conflicting bytes");
        }
      }
      require(copies == 1, "owned conflict merge duplicated or lost its sibling");
    } else require(text(read_bytes(directory / (info["transition"].get<bool>() ? "Source/item/leaf" : "Source/item"))) == "after", "owned recovery restored wrong bytes");
    if (info["scope"] == "partial") require(!fs::exists(directory / "Content"), "owned recovery expanded a partial workspace");
    store.gc(true); store.fsck();
  }
  return Json{{"verified", true}, {"pending", pending["pending"]}};
}
}
namespace syrnck::testing {
void io_checkpoint(std::string_view event, const fs::path &path) {
  const auto filename = utf8(path.filename());
  const auto relative = path.lexically_relative(fixture_root / "primary/.syrnck/objects");
  const auto category = filename == "refs.json" ? "refs" : filename == "config.json" ? "config"
      : !relative.empty() && *relative.begin() != ".." ? utf8(*relative.begin()) : "filesystem";
  if (tracing) { events.push_back(Json{{"phase", event}, {"kind", category}, {"occurrence", ++counts[{std::string(event), category}]}}); return; }
  if (!armed || event != phase || category != kind || ++seen != occurrence) return;
  armed = false; std::cout << Json{{"phase", phase}, {"kind", kind}, {"occurrence", seen}}.dump() << std::endl;
  std::string line; require(static_cast<bool>(std::getline(std::cin, line)) && line == "continue", "owned checkout checkpoint closed");
}
}
int main(int argc, char **argv) {
  try {
    require(argc >= 3, "owned checkout probe expects mode and root");
    const auto root = path_from_utf8(argv[2]); fixture_root = root; const std::string mode = argv[1];
    if (mode == "prepare") { require(argc == 7, "owned checkout probe expects encryption scope action transition"); prepare(root, std::string_view(argv[3]) == "encrypted", argv[4], argv[5], std::string_view(argv[6]) == "transition"); }
    else {
      auto info = parse_json(read_bytes(root / "fixture.json"));
      server_base = info.value("endpoint", server_base);
      // Bind copied, portable seeds before arming any termination point. This
      // avoids another process launch without omitting a tested IO boundary.
      if ((mode == "trace" || mode == "run") && !info.contains("owner")) {
        bind(root, info); info = parse_json(read_bytes(root / "fixture.json"));
      }
      if (mode == "bind") bind(root, info);
      else {
        Store store(root / "primary/.syrnck", encryption(info)); select(store, info);
        if (mode == "trace" || mode == "run") {
          if (mode == "trace") tracing = true;
          else { require(argc == 6, "owned checkout probe expects phase kind occurrence"); phase = argv[3]; kind = argv[4]; occurrence = std::stoull(argv[5]); armed = true; }
          perform(store, root, info);
          if (tracing) std::cout << events.dump() << '\n'; else require(!armed, "owned checkout checkpoint not reached");
        } else if (mode == "refuse") {
          refuse(store, root, info);
          std::cout << Json{{"verified", true}}.dump() << '\n';
        } else {
          require(mode == "verify" || mode == "resume", "unknown owned checkout probe mode");
          bool without_key = false;
          if (mode == "resume") without_key = perform(store, root, info);
          auto result = verify(store, root, info, mode == "resume"); result["resumed_without_key"] = without_key;
          std::cout << result.dump() << '\n';
        }
      }
    }
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
