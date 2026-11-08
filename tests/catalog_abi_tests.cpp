#include "native_history_fixture.hpp"
#include <syrnck.h>
#include <array>
#include <iostream>
using namespace native_test;
namespace {
void success(int code) { require(code == 0, syrnck_last_error()); }
struct Handle {
  SyrnckStore *value = nullptr;
  Handle(const fs::path &root, const std::optional<Key> &key) {
    success(syrnck_store_open(utf8(root / ".syrnck").c_str(), false,
        key ? key->data() : nullptr, key ? key->size() : 0, &value));
  }
  ~Handle() { syrnck_store_close(value); }
};
struct Cancellation {
  SyrnckCancellation *value = nullptr;
  Cancellation() { success(syrnck_cancellation_create(&value)); }
  ~Cancellation() { syrnck_cancellation_close(value); }
};
Json refs(SyrnckStore *store) {
  std::size_t length = 0;
  success(syrnck_refs_json(store, nullptr, 0, &length));
  std::vector<char> data(length);
  success(syrnck_refs_json(store, data.data(), data.size(), &length));
  require(length == data.size() && data.back() == 0, "reference table ABI length differs");
  return parse_json(bytes(std::string_view(data.data(), length - 1)));
}
std::string put(SyrnckStore *store, std::string_view kind, std::string_view data) {
  std::array<char, 65> id{};
  success(syrnck_object_put(store, std::string(kind).c_str(), bytes(data).data(), data.size(), id.data()));
  return id.data();
}
int compare(SyrnckStore *store, const Json &updates, int &applied, const SyrnckOperation *operation = nullptr) {
  const auto encoded = updates.dump();
  return syrnck_refs_compare_json(store, bytes(encoded).data(), encoded.size(), operation, &applied);
}
struct Stop {
  SyrnckCancellation *cancellation;
  std::uint32_t phase;
  bool seen = false;
  static void report(const SyrnckProgress *event, void *user) {
    auto &state = *static_cast<Stop *>(user);
    if (event->phase == state.phase && (state.phase == SYRNCK_PHASE_PUBLISHING || event->bytes > 0)) {
      state.seen = true;
      success(syrnck_cancellation_request(state.cancellation));
    }
  }
};
void scenario(const fs::path &project, std::optional<Key> key, unsigned layout) {
  std::string manifest, signed_id, signed_document;
  const std::string content(2 * 1024 * 1024 + 31, 'k');
  {
    Fixture fixture(project, key);
    manifest = fixture.file(content)["manifest"].get<std::string>();
    fixture.store.set_ref("keep", {Kind::manifest, manifest});
    Key seed; seed.fill(69);
    fixture.store.use_native_identity({"SDK author", "sdk@example.invalid", 0}, SigningKey::from_seed(seed));
    const auto signed_value = fixture.store.native_commit_metadata(Json{{"version", 1}, {"parents", Json::array()},
        {"message", "Signed SDK <&>"}, {"timestamp", 1},
        {"files", Json{{"data/asset", Json{{"manifest", manifest}, {"executable", false}}}}}});
    signed_id = object_id(Kind::revision, bytes(signed_value.dump()));
    signed_document = signed_value.dump(2);
    fixture.pack(layout);
  }
  {
    Handle store(project, key);
    const auto original = refs(store.value);
    const auto physical_refs = read_bytes(project / ".syrnck/refs.json");
    require(put(store.value, "revision", signed_document) == signed_id,
        "SDK canonicalization changed a valid authored signature or identity");
    std::size_t length = 999;
    require(syrnck_refs_json(nullptr, nullptr, 0, &length) == 1 && length == 0 &&
        syrnck_refs_json(store.value, nullptr, 0, nullptr) == 1, "reference query accepted invalid outputs");
    std::array<char, 2> small{'x', 'y'};
    require(syrnck_refs_json(store.value, small.data(), small.size(), &length) == 1 && length > small.size() &&
        small == std::array<char, 2>{'x', 'y'}, "short reference query changed caller memory");
    const SyrnckChunking fixed{1, 1024, 1024, 1024};
    std::array<char, 65> id{};
    success(syrnck_ingest_buffer_ex(store.value, bytes(content).data(), content.size(), &fixed, nullptr, id.data()));
    require(std::string(id.data()) == manifest && refs(store.value) == original &&
        read_bytes(project / ".syrnck/refs.json") == physical_refs,
        "memory preparation published an implicit root or changed content identity");
    success(syrnck_ingest_buffer_ex(store.value, nullptr, 0, nullptr, nullptr, id.data()));
    success(syrnck_file_verify(store.value, id.data()));
    require(refs(store.value) == original, "empty memory preparation pinned data");
    if (!key && layout == 0) {
      const auto path = project / "large-input";
      atomic_write(path, {}, false);
      fs::resize_file(path, max_object_size + 17);
      success(syrnck_prepare_file_ex(store.value, utf8(path).c_str(), nullptr, nullptr, id.data()));
      success(syrnck_file_verify(store.value, id.data()));
      std::array<unsigned char, 17> tail; tail.fill(1);
      success(syrnck_file_read_range(store.value, id.data(), max_object_size, tail.size(), tail.data(), tail.size(), &length));
      require(length == tail.size() && std::all_of(tail.begin(), tail.end(), [](auto b) { return b == 0; }) &&
          refs(store.value) == original, "streaming preparation truncated or retained a file over 16 MiB");
      require(syrnck_prepare_file_ex(store.value, nullptr, nullptr, nullptr, id.data()) == 1 && id[0] == 0,
          "stream preparation accepted a null path");
    }
    const Json typed{{"kind", "revision"}, {"id", signed_id}};
    int applied = 7;
    const Json publication = Json::array({Json{{"name", "ffi/catalog/main"}, {"expected", nullptr}, {"value", typed}},
        Json{{"name", "ffi/catalog/archive"}, {"expected", nullptr}, {"value", typed}}});
    {
      Cancellation cancellation;
      Stop stop{cancellation.value, SYRNCK_PHASE_PUBLISHING};
      const SyrnckOperation operation{cancellation.value, Stop::report, &stop};
      require(compare(store.value, publication, applied, &operation) == SYRNCK_CANCELLED && applied == 0 && stop.seen &&
          refs(store.value) == original && read_bytes(project / ".syrnck/refs.json") == physical_refs,
          "cancelled atomic publication changed any group member");
    }
    success(compare(store.value, publication, applied));
    require(applied == 1, "SDK could not atomically publish a complete authored catalog");
    const auto committed = refs(store.value);
    const auto committed_bytes = read_bytes(project / ".syrnck/refs.json");
    success(compare(store.value, publication, applied));
    require(applied == 0 && refs(store.value) == committed &&
        read_bytes(project / ".syrnck/refs.json") == committed_bytes, "stale SDK CAS rewrote references");
    auto invalid = publication; invalid[1] = invalid[0];
    require(compare(store.value, invalid, applied) == 1 && applied == 0 && refs(store.value) == committed,
        "SDK accepted a duplicate reference transaction member");
    for (const auto &document : {std::string("[] trailing"), std::string("[{\"name\":\"x\",\"name\":\"y\",\"expected\":null,\"value\":null}]"),
        std::string("[{\"name\":\"x\",\"expected\":null,\"value\":null,\"extra\":1}]")}) {
      applied = 7;
      require(syrnck_refs_compare_json(store.value, bytes(document).data(), document.size(), nullptr, &applied) == 1 &&
          applied == 0 && refs(store.value) == committed, "ambiguous reference input changed refs");
    }
    for (const auto &document : {std::string("{\"version\":1,\"version\":1}"),
        std::string("{\"version\":1,\"ver\\u0073ion\":1}"), std::string(129, '[') + std::string(129, ']')}) {
      id.fill('x');
      require(syrnck_object_put(store.value, "revision", bytes(document).data(), document.size(), id.data()) == 1 && id[0] == 0,
          "SDK metadata input accepted duplicate fields or excess depth");
    }
    require(syrnck_object_put(store.value, "chunk", nullptr, 1, id.data()) == 1 && id[0] == 0 &&
        syrnck_object_put(store.value, "unknown", bytes("a").data(), 1, id.data()) == 1 &&
        syrnck_object_put(store.value, "chunk", bytes("a").data(), max_object_size + 1, id.data()) == 1,
        "SDK object creation accepted invalid memory or types");
    const SyrnckChunking bad_chunking{2, 1, 1, 1};
    require(syrnck_ingest_buffer_ex(store.value, nullptr, 1, nullptr, nullptr, id.data()) == 1 &&
        syrnck_ingest_buffer_ex(store.value, bytes("a").data(), 1, &bad_chunking, nullptr, id.data()) == 1,
        "SDK memory preparation accepted invalid data/chunking");
    {
      Cancellation cancellation;
      Stop stop{cancellation.value, SYRNCK_PHASE_INGESTING};
      const SyrnckOperation operation{cancellation.value, Stop::report, &stop};
      require(syrnck_ingest_buffer_ex(store.value, bytes(content).data(), content.size(), &fixed, &operation, id.data()) ==
          SYRNCK_CANCELLED && stop.seen && id[0] == 0 && refs(store.value) == committed,
          "active memory preparation ignored cancellation or pinned partial content");
    }
    success(compare(store.value, Json::array({Json{{"name", "ffi/catalog/main"}, {"expected", typed}, {"value", nullptr}}}), applied));
    require(applied == 1 && !refs(store.value).contains("ffi/catalog/main"), "SDK removal failed");
  }
  Store store(project / ".syrnck", key);
  store.gc(true); store.fsck();
  store.export_revision(signed_id, project / "recovered");
  require(text(read_bytes(project / "recovered/data/asset")) == content,
      "atomic catalog/GC lost retained signed data");
}
}
int main() {
  Temporary temporary;
  try {
    for (bool encrypted : {false, true}) for (unsigned layout = 0; layout < 3; ++layout) {
      std::optional<Key> key; if (encrypted) { key.emplace(); key->fill(68); }
      scenario(temporary.path / (std::to_string(encrypted) + '-' + std::to_string(layout)), key, layout);
    }
    std::cout << "Catalog C ABI: canonical objects, unpinned preparation, atomic references, cancellation and GC\n";
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
