#include "native_history_fixture.hpp"
#include <syrnck.h>
#include <iostream>
using namespace native_test;
namespace {
void success(int code) { require(code == 0, syrnck_last_error()); }
struct Handle {
  SyrnckStore *value = nullptr;
  ~Handle() { syrnck_store_close(value); }
};
Json identity(SyrnckStore *store) {
  char *out = nullptr; std::size_t length = 0;
  success(syrnck_store_native_identity_json(store, &out, &length));
  require(out && length && out[length - 1] == 0, "invalid public identity output");
  const auto result = parse_unique_json(bytes(std::string_view(out, length - 1)));
  syrnck_string_free(out); return result;
}
struct Cancel {
  SyrnckCancellation *source = nullptr;
  SyrnckStore *store;
  bool seen = false, refused = false;
  explicit Cancel(SyrnckStore *value) : store(value) { success(syrnck_cancellation_create(&source)); }
  ~Cancel() { syrnck_cancellation_close(source); }
  static void progress(const SyrnckProgress *event, void *user) {
    auto &state = *static_cast<Cancel *>(user);
    if (event->phase != SYRNCK_PHASE_VERIFYING || event->bytes == 0) return;
    state.seen = true;
    state.refused = syrnck_store_use_native_identity(state.store, "Changed", "changed@example.invalid", 0, nullptr, 0, false) != 0;
    success(syrnck_cancellation_request(state.source));
  }
};
void scenario(const fs::path &root, bool encrypted) {
  std::optional<Key> key;
  if (encrypted) { key.emplace(); key->fill(83); }
  std::string signed_id;
  {
    Handle store;
    success(syrnck_store_open(utf8(root).c_str(), true, key ? key->data() : nullptr, key ? key->size() : 0, &store.value));
    require(identity(store.value).is_null(), "new handle has an identity");
    std::array<unsigned char,32> seed; seed.fill(28);
    success(syrnck_store_use_native_identity(store.value, "Équipe ABI", "abi@example.invalid", -120, seed.data(), seed.size(), false));
    seed.fill(0);
    const auto original = identity(store.value);
    require(original.at("timezone_offset") == -120 && original.at("public_key").is_string(), "public identity differs");
    require(syrnck_store_use_native_identity(store.value, nullptr, "email", 0, nullptr, 0, false) != 0 &&
        syrnck_store_use_native_identity(store.value, "name", "email", 1440, nullptr, 0, false) != 0 &&
        syrnck_store_use_native_identity(store.value, "name", "email", 0, nullptr, 32, false) != 0 &&
        syrnck_store_use_native_identity(store.value, "name", "email", 0, seed.data(), 31, false) != 0,
        "invalid identity inputs accepted");
    require(identity(store.value) == original, "invalid input changed active identity");
    char *output = reinterpret_cast<char *>(1); std::size_t length = 9;
    require(syrnck_store_native_identity_json(nullptr, &output, &length) != 0 && !output && length == 0,
        "failed identity output not reset");
    const std::string contents(2 * 1024 * 1024 + 7, 'r');
    char manifest[65], revision[65];
    success(syrnck_ingest_buffer_ex(store.value, reinterpret_cast<const unsigned char *>(contents.data()), contents.size(), nullptr, nullptr, manifest));
    const Json draft{{"version",1},{"parents",Json::array()},{"message","prepared ABI"},{"timestamp",42},
        {"files",Json{{"asset",Json{{"manifest",manifest},{"executable",false}}}}}};
    const auto encoded = draft.dump();
    Cancel cancellation(store.value);
    const SyrnckOperation operation{cancellation.source, Cancel::progress, &cancellation};
    const auto cancelled = syrnck_prepare_native_revision_ex(store.value, reinterpret_cast<const unsigned char *>(encoded.data()), encoded.size(), &operation, revision);
    require(cancelled == SYRNCK_CANCELLED && !revision[0],
        "closure cancellation status " + std::to_string(cancelled) + ": " + syrnck_last_error());
    require(cancellation.seen && cancellation.refused && identity(store.value) == original,
        "progress callback changed identity or cancellation was not observed");
    success(syrnck_prepare_native_revision_ex(store.value, reinterpret_cast<const unsigned char *>(encoded.data()), encoded.size(), nullptr, revision));
    signed_id = revision;
    auto missing = draft; missing["files"]["asset"]["manifest"] = std::string(64,'f');
    const auto invalid = missing.dump();
    require(syrnck_prepare_native_revision_ex(store.value, reinterpret_cast<const unsigned char *>(invalid.data()), invalid.size(), nullptr, revision) != 0 && !revision[0],
        "missing dependency was not refused");
    require(syrnck_prepare_native_revision_ex(store.value, nullptr, 0, nullptr, revision) != 0 && !revision[0] &&
        syrnck_prepare_native_revision_ex(store.value, nullptr, 0, nullptr, nullptr) != 0, "invalid revision input accepted");
    success(syrnck_store_use_native_identity(store.value, "Required", "required@example.invalid", 0, nullptr, 0, true));
    require(identity(store.value)["signature_required"] == true, "required signature policy missing from public identity");
    require(syrnck_prepare_native_revision_ex(store.value, reinterpret_cast<const unsigned char *>(encoded.data()), encoded.size(), nullptr, revision) != 0 && !revision[0],
        "required signing key silently downgraded");
    success(syrnck_store_clear_native_identity(store.value));
    require(identity(store.value).is_null(), "cleared identity remained active");
  }
  Store reopened(root,key);
  require(reopened.native_identity().is_null() && reopened.refs().empty(), "identity or preparation persisted a root");
  const auto value = reopened.metadata(Kind::revision,signed_id);
  require(value["version"] == 2 && value["author"]["name"] == "Équipe ABI" && value["signature"]["algorithm"] == "ed25519",
      "ABI prepared revision lost native signature");
  (void)reopened.closure({ObjectKey{Kind::revision,signed_id}},true);
}
}
int main() {
  Temporary temporary;
  try {
    for (bool encrypted : {false,true}) scenario(temporary.path / std::to_string(encrypted),encrypted);
    std::cout << "Native author/signature C ABI with cancellation and handle-local secret ownership\n";
  } catch (const std::exception &error) { std::cerr << error.what() << '\n';return 1; }
}
