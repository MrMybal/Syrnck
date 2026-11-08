#include "native_history_fixture.hpp"
#include "native_authorship.hpp"
#include <iostream>
using namespace native_test;
namespace {
bool armed = false, tracing = false;
std::string checkpoint, kind;
std::size_t occurrence = 0, seen = 0;
Json events = Json::array();
std::map<std::pair<std::string, std::string>, std::size_t> counts;
fs::path fixture_root;
std::optional<Key> encryption(const Json &info) { if (!info["encrypted"].get<bool>()) return {}; Key value; value.fill(88); return value; }
SigningKey signer(unsigned value) { Key seed; seed.fill(static_cast<std::uint8_t>(value)); return SigningKey::from_seed(seed); }
void bob(Store &store) { store.use_native_identity({"Bob", "bob@example.invalid", -60}, signer(32)); }
std::string category(const fs::path &path) {
  const auto name = utf8(path.filename());
  if (name == "refs.json") return "refs";
  if (name == "config.json") return "config";
  const auto relative = path.lexically_relative(fixture_root / "primary/.syrnck/objects");
  if (!relative.empty() && *relative.begin() != "..") return utf8(*relative.begin());
  return "filesystem";
}
void write(const fs::path &root, std::string_view path, std::string_view data) {
  const auto destination = root / path_from_utf8(path); fs::create_directories(destination.parent_path()); atomic_write(destination, bytes(data), true);
}
fs::path working(const fs::path &root, const Json &info) { return root / (info["scope"] == "primary" ? "primary" : "edit"); }
void select(Store &store, const Json &info) { store.use_native_workspace(info["scope"] == "primary" ? "" : "edit"); }
void prepare(const fs::path &root, bool encrypted, std::string scope, std::string action) {
  Fixture f(root / "primary", encryption(Json{{"encrypted", encrypted}}));
  auto files = Json{{".syrnckignore", f.file("*.keep\n")}, {"Content/asset", f.file("hidden")},
      {"Source/code", f.file("base")}, {"Source/stable", f.file("before")}};
  const auto base = f.revision(files);
  files["Source/stable"] = f.file("after"); files["Source/onto"] = f.file("new base");
  const auto onto = f.revision(files, Json::array({base}));
  f.install(base);
  atomic_write(root / "fixture.json", bytes(Json{{"encrypted", encrypted}, {"scope", scope}, {"action", action},
      {"base", base}, {"onto", onto}}.dump()), false);
}
void bind(const fs::path &root, Json info) {
  Store store(root / "primary/.syrnck", encryption(info));
  auto old = info["base"].get<std::string>();
  if (info["action"] != "commit") {
    store.use_native_identity({"Alice", "alice@example.invalid", 60}, signer(31));
    auto draft = store.metadata(Kind::revision, old);
    std::istringstream input("picked"); const auto manifest = store.ingest(input, Chunking::fixed_size(1024)).first;
    draft["files"]["Source/code"]["manifest"] = manifest; draft["message"] = "original author"; draft["timestamp"] = 10;
    draft["parents"] = Json::array({old});
    old = store.put(Kind::revision, bytes(store.native_commit_metadata(std::move(draft)).dump())).first.id;
  }
  const auto directory = working(root, info);
  if (info["scope"] != "primary") {
    if (info["scope"] == "partial") store.register_partial_native_workspace(root / "primary", "edit", directory, {"Source"});
    else store.register_native_workspace(root / "primary", "edit", directory);
    if (info["scope"] != "detached") store.create_native_branch("feature", old);
    store.use_native_workspace("edit");
    if (info["scope"] == "detached") store.detach_checkout(directory, old); else store.switch_directory(directory, "feature");
  } else {
    store.set_ref("heads/main", {Kind::revision, old});
    if (info["action"] != "commit") write(directory, "Source/code", "picked");
  }
  info["old"] = old; info["head_ref"] = native_revision_reference(store, store.native_target());
  if (info["action"] == "commit") { write(directory, "Source/code", "committed"); write(directory, "Source/new", "new working file"); }
  write(directory, "local.keep", "unrelated ignored work");
  bob(store); info["bob_key"] = store.native_identity()["signing_key"];
  if (info["action"] == "rebase" || info["action"] == "abort") {
    NativeRebaseOptions settings; settings.committer_timestamp = 1234;
    const auto plan = store.plan_rebase(directory, store.native_target(), info["base"].get<std::string>(), info["onto"].get<std::string>(), settings);
    require(plan["ready"] == true, "authored rebase fixture is blocked: " + plan.dump()); info["target"] = plan["target"];
  }
  store.pack(); atomic_write(root / "fixture.json", bytes(info.dump()), true);
}
void perform(Store &store, const fs::path &root, const Json &info, bool recovery) {
  select(store, info);
  const auto status = store.checkout_status();
  if (status["pending"].get<bool>()) {
    require(store.native_identity().is_null(), "fresh checkout recovery unexpectedly loaded a signing key");
    (void)store.recover_checkout(recovery && info["action"] == "abort" && status["state"] != "committed"); return;
  }
  const auto head = store.native_head()["revision"].get<std::string>();
  if (head != info["old"].get<std::string>() || (recovery && info["action"] == "abort")) return;
  bob(store);
  const auto directory = working(root, info);
  const auto target = store.native_target();
  if (info["action"] == "commit") store.commit_directory(directory, target, "signed commit");
  else if (info["action"] == "amend") store.amend_message(target, head, "signed amendment");
  else {
    NativeRebaseOptions settings; settings.committer_timestamp = 1234;
    store.rebase_directory(directory, target, head, info["base"].get<std::string>(), info["onto"].get<std::string>(), settings);
  }
}
Json verify(Store &store, const fs::path &root, const Json &info, bool recovered) {
  if (recovered) store.gc(true);
  store.fsck(); select(store, info);
  const auto status = store.checkout_status();
  const auto head = store.native_head()["revision"].get<std::string>();
  const bool changed = head != info["old"].get<std::string>();
  const auto value = store.metadata(Kind::revision, head);
  require(text(read_bytes(working(root, info) / "local.keep")) == "unrelated ignored work", "authored operation changed unrelated work");
  if (changed) {
    require(value["version"] == 2 && value["committer"]["name"] == "Bob" && value["signature"]["key_id"] == info["bob_key"],
        "authored publication lost committer or signing key");
    require(value["author"]["name"] == (info["action"] == "commit" ? "Bob" : "Alice"), "authored rewrite changed original author");
    if (info["action"] == "rebase" || info["action"] == "abort")
      require(head == info["target"].get<std::string>() && value["timestamp"] == 1234, "rebase published an unfrozen candidate");
  }
  const auto previous = store.metadata(Kind::revision, info["old"].get<std::string>());
  (void)store.closure({{Kind::revision, head}, {Kind::revision, info["old"].get<std::string>()}}, true);
  if (status["pending"].get<bool>()) require(status["target"] == info["target"], "pending checkout lost its signed target");
  if (recovered) {
    require(status["pending"] == false && (changed || info["action"] == "abort"), "authored operation did not finish recovery");
    require(store.contains({Kind::revision, info["old"].get<std::string>()}), "GC lost the former signed history");
    if (info["action"] == "rebase" || info["action"] == "abort")
      require(text(read_bytes(working(root, info) / "Source/stable")) == (changed ? "after" : "before"), "authored checkout recovery restored wrong files");
    if (changed && info["action"] == "amend") require(previous["author"] == value["author"], "amendment lost exact original author stamp");
  }
  return Json{{"verified", true}, {"changed", changed}, {"pending", status["pending"]}};
}
}
namespace syrnck::testing {
void io_checkpoint(std::string_view event, const fs::path &path) {
  const auto type = category(path);
  if (tracing) { events.push_back(Json{{"phase", event}, {"kind", type}, {"occurrence", ++counts[{std::string(event), type}]}}); return; }
  if (!armed || event != checkpoint || type != kind || ++seen != occurrence) return;
  armed = false; std::cout << Json{{"phase", checkpoint}, {"kind", kind}, {"occurrence", seen}}.dump() << std::endl;
  std::string line; require(static_cast<bool>(std::getline(std::cin, line)) && line == "continue", "authorship checkpoint closed");
}
}
int main(int argc, char **argv) {
  try {
    require(argc >= 3, "authorship probe expects mode and root");
    const auto root = path_from_utf8(argv[2]); fixture_root = root; const std::string mode = argv[1];
    if (mode == "prepare") {
      require(argc == 6, "authorship probe expects encryption scope action");
      prepare(root, std::string_view(argv[3]) == "encrypted", argv[4], argv[5]);
    } else {
      const auto info = parse_json(read_bytes(root / "fixture.json"));
      if (mode == "bind") bind(root, info);
      else {
        Store store(root / "primary/.syrnck", encryption(info));
        if (mode == "trace" || mode == "run") {
          if (mode == "trace") tracing = true;
          else { require(argc == 6, "authorship probe expects checkpoint kind occurrence"); checkpoint = argv[3]; kind = argv[4]; occurrence = std::stoull(argv[5]); armed = true; }
          perform(store, root, info, false);
          if (tracing) std::cout << events.dump() << '\n'; else require(!armed, "authorship checkpoint was not reached");
        } else {
          require(mode == "verify" || mode == "resume", "unknown authorship probe mode");
          if (mode == "resume") perform(store, root, info, true);
          std::cout << verify(store, root, info, mode == "resume").dump() << '\n';
        }
      }
    }
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
