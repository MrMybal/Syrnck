#include "pack_storage.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
using namespace syrnck;

namespace {
void check(bool value, std::string_view message = "pack test failed") {
  require(value, message);
}
template <class Function> void fails(Function function) {
  bool failed = false;
  try {
    function();
  } catch (const std::exception &) {
    failed = true;
  }
  check(failed, "expected failure");
}
fs::path loose(const Store &store, const ObjectKey &key) {
  return store.root() / "objects" / kind_name(key.kind) / key.id.substr(0, 2) / key.id;
}
Json config(const fs::path &root) {
  return parse_json(read_bytes(root / "config.json"));
}
fs::path pack_file(const fs::path &root, std::string_view extension) {
  return root / "packs" /
         path_from_utf8("pack-" + config(root).at("pack").at("name").get<std::string>() +
                        std::string(extension));
}
std::string sample(std::size_t size, std::uint64_t state = 0x123456789abcdef0ULL) {
  std::string result(size, '\0');
  for (auto &c : result) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    c = static_cast<char>(state);
  }
  return result;
}
std::string ingest(Store &store, const std::string &data, bool pin = true) {
  std::istringstream source(data);
  auto id = store.ingest(source, Chunking::fixed_size(4096), {.workers = 4}).first;
  if (pin)
    store.set_ref("files/" + id, {Kind::manifest, id});
  return id;
}
void restore(Store &store, const std::string &id, const std::string &data) {
  std::ostringstream out;
  store.reconstruct(id, out);
  check(out.str() == data, "reconstructed bytes differ");
}
void rewrite_index(const fs::path &root, const std::function<void(Bytes &)> &mutate) {
  auto file = pack_file(root, ".syi");
  auto index = read_bytes(file);
  mutate(index);
  atomic_write(file, index, true);
  auto document = config(root);
  document["pack"]["index_hash"] = hash(index);
  atomic_write(root / "config.json", bytes(document.dump()), true);
}
} // namespace

int main() {
  auto root = fs::temp_directory_path() / path_from_utf8("syrnck-packs-" + hex(random_bytes(16)));
  fs::create_directory(root);
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code ec;
      fs::remove_all(path, ec);
    }
  } cleanup{root};
  std::size_t count = 0;
  auto test = [&](std::string_view name, const auto &function) {
    function();
    ++count;
    std::cout << "PASS " << name << '\n';
  };
  try {
    test("plain/encrypted round trips, byte-preserved envelopes, mixed writes and reopen", [&] {
      for (bool encrypted : {false, true}) {
        auto directory = root / (encrypted ? "encrypted" : "plain");
        std::optional<Key> key;
        if (encrypted) {
          key.emplace();
          key->fill(17);
        }
        auto data = sample(128 * 1024 + 13);
        std::string id, second;
        std::vector<ObjectKey> keys;
        {
          Store store(directory, key, true);
          id = ingest(store, data);
          keys = store.objects();
          std::map<ObjectKey, Bytes> envelopes;
          for (const auto &item : keys)
            envelopes.emplace(item, read_bytes(loose(store, item)));
          check(config(directory)["version"] == 1);
          auto result = store.pack();
          check(result["objects"] == keys.size() && result["loose_objects_removed"] == keys.size());
          check(config(directory)["version"] == 2 && store.objects() == keys);
          PackStorage packs(directory);
          packs.load(config(directory)["pack"]);
          for (const auto &item : keys) {
            check(!fs::exists(loose(store, item)));
            check(packs.read(item) == envelopes.at(item), "packing changed an envelope");
            check(store.contains(item));
          }
          restore(store, id, data);
          check(ingest(store, data) == id && store.objects() == keys);
          second = ingest(store, data + "new tail");
          check(second != id);
          restore(store, second, data + "new tail");
          check(store.fsck() > keys.size());
        }
        {
          Store store(directory, key);
          restore(store, id, data);
          restore(store, second, data + "new tail");
          check(store.gc()["unreachable_objects"] == 0);
          auto result = store.pack();
          check(result["obsolete_pack_files_removed"] == 0 && result["pack_count"] == 2);
          check(config(directory)["version"] == 3);
          check(store.fsck() == store.objects().size());
        }
        if (encrypted) {
          fails([&] { Store wrong(directory); });
          Key wrong{};
          fails([&] { Store store(directory, wrong); });
        }
      }
    });
    test("packed GC retains shared blocks and rewrites only after validating roots", [&] {
      Store store(root / "gc", {}, true);
      auto shared = sample(4096);
      auto first = shared + sample(4096, 42);
      auto second = shared + sample(4096, 43);
      auto old = ingest(store, first), current = ingest(store, second);
      store.pack();
      auto active = pack_file(store.root(), ".syp");
      auto before = read_bytes(active);
      store.remove_ref("files/" + old);
      auto dry = store.gc();
      check(dry["unreachable_objects"] == 2 && dry["repack_required"] == true);
      check(read_bytes(active) == before && fs::exists(active));
      auto collected = store.gc(true);
      check(collected["unreachable_objects"] == 2 && !fs::exists(active));
      restore(store, current, second);
      fails([&] { restore(store, old, first); });
      check(store.fsck() == 3 && store.gc()["unreachable_objects"] == 0);
      store.remove_ref("files/" + current);
      check(store.gc(true)["unreachable_objects"] == 3);
      check(config(store.root())["pack"].is_null() && store.objects().empty());
      check(store.fsck() == 0);
    });
    test("missing live blocks forbid GC and pack without deleting any stored bytes", [&] {
      Store store(root / "missing-live", {}, true);
      auto id = ingest(store, sample(8192));
      ingest(store, sample(4096, 82), false);
      store.pack();
      auto file = pack_file(store.root(), ".syp");
      auto before = read_bytes(file);
      auto refs = config(store.root());
      auto bogus = Json{{"broken", key_json({Kind::chunk, std::string(64, 'a')})}}.dump();
      atomic_write(store.root() / "refs.json", bytes(bogus), true);
      fails([&] { store.gc(true); });
      fails([&] { store.pack(); });
      check(read_bytes(file) == before && config(store.root()) == refs);
      atomic_write(store.root() / "refs.json",
                   bytes(Json{{"files/" + id, key_json({Kind::manifest, id})}}.dump()), true);
      check(store.gc(true)["unreachable_objects"] == 2);
    });
    test("warm cache detects changed packed envelope and restore stays atomic", [&] {
      for (bool encrypted : {false, true}) {
        std::optional<Key> key;
        if (encrypted) {
          key.emplace();
          key->fill(19);
        }
        Store store(root / (encrypted ? "tamper-encrypted" : "tamper-plain"), key, true);
        auto data = sample(16384);
        auto id = ingest(store, data);
        store.pack();
        restore(store, id, data);
        auto file = pack_file(store.root(), ".syp");
        auto original = read_bytes(file);
        auto changed = original;
        changed[15] ^= 1; // Inside the first chunk envelope, length unchanged.
        auto time = fs::last_write_time(file);
        atomic_write(file, changed, true);
        fs::last_write_time(file, time);
        auto chunk = store.objects().front();
        fails([&] { store.get(chunk); });
        fails([&] { store.contains(chunk); });
        fails([&] { store.fsck(); });
        fails([&] { store.gc(true); });
        auto destination = store.root() / "should-not-exist";
        fails([&] { store.restore_file(id, destination); });
        check(!fs::exists(destination));
        atomic_write(file, original, true);
        restore(store, id, data);
      }
    });
    test("loose duplicates cannot hide active pack corruption from fsck", [&] {
      Store store(root / "duplicates", {}, true);
      auto id = ingest(store, sample(8192));
      auto chunk = store.objects().front();
      auto envelope = read_bytes(loose(store, chunk));
      store.pack();
      atomic_write(loose(store, chunk), envelope, false);
      auto file = pack_file(store.root(), ".syp");
      auto data = read_bytes(file);
      data[15] ^= 1;
      atomic_write(file, data, true);
      check(!store.get(chunk).empty()); // Normal precedence is the loose copy.
      fails([&] { store.fsck(); });
      fails([&] { store.pack(); });
      check(fs::exists(loose(store, chunk)) && fs::exists(file));
      (void)id;
    });
    test("malformed/truncated indexes fail closed, even with a matching index hash", [&] {
      for (int mode = 0; mode < 9; ++mode) {
        auto directory = root / ("bad-index-" + std::to_string(mode));
        {
          Store store(directory, {}, true);
          ingest(store, sample(8192));
          store.pack();
        }
        rewrite_index(directory, [&](Bytes &index) {
          switch (mode) {
          case 0:
            index[0] ^= 1;
            break; // Magic
          case 1:
            index[8] = 0;
            break; // Count disagrees
          case 2:
            index[16] = 3;
            break; // Unknown kind
          case 3:
            index[16 + 33] = 0;
            break; // Range before payload
          case 4:
            index[16 + 41] = 0xff;
            index[16 + 44] = 0x7f;
            break; // Oversized
          case 5:
            std::copy_n(index.begin() + 16, 45, index.begin() + 61);
            break; // Duplicate
          case 6:
            index.pop_back();
            break;
          case 7:
            index.push_back(0);
            break;
          case 8:
            index[16 + 33 + 7] = 0x80;
            break; // Overflowing offset
          }
        });
        fails([&] { Store store(directory); });
        check(fs::exists(pack_file(directory, ".syp")));
      }
    });
    test("pack truncation, trailing data, bad header and missing index are rejected", [&] {
      for (int mode = 0; mode < 5; ++mode) {
        auto directory = root / ("bad-pack-" + std::to_string(mode));
        Store store(directory, {}, true);
        ingest(store, sample(8192));
        store.pack();
        auto file = pack_file(directory, mode == 4 ? ".syi" : ".syp");
        auto original = read_bytes(file);
        auto changed = original;
        if (mode == 0)
          changed.pop_back();
        if (mode == 1)
          changed.push_back(0);
        if (mode == 2)
          changed[0] ^= 1;
        if (mode == 3 || mode == 4)
          fs::remove(file);
        else
          atomic_write(file, changed, true);
        fails([&] { store.fsck(); });
        fails([&] { store.gc(true); });
        fails([&] { store.get(store.refs().begin()->second); }); // Failed reload poisons the index.
        atomic_write(file, original, fs::exists(file));
        check(store.fsck() == 3);
      }
    });
    test("unpublished generations are harmless and cleaned after validation", [&] {
      for (bool already_packed : {false, true}) {
        auto directory = root / (already_packed ? "orphan-v2" : "orphan-v1");
        auto data = sample(8192);
        std::string id;
        fs::path orphan;
        {
          Store store(directory, {}, true);
          id = ingest(store, data);
          auto keys = store.objects();
          std::map<ObjectKey, Bytes> envelopes;
          for (const auto &key : keys)
            envelopes.emplace(key, read_bytes(loose(store, key)));
          if (already_packed)
            store.pack();
          auto next =
              PackStorage::build(directory, keys, [&](const ObjectKey &key) { return envelopes.at(key); });
          orphan = directory / "packs" /
                   path_from_utf8("pack-" + next.metadata()["name"].get<std::string>() + ".syp");
          check(store.fsck() == 3);
        }
        Store store(directory);
        restore(store, id, data);
        check(store.gc()["obsolete_pack_files"] == 2 && fs::exists(orphan));
        store.gc(true);
        check(!fs::exists(orphan));
        restore(store, id, data);
      }
    });
    test("recovery after publication with both loose and previous packed copies remaining", [&] {
      auto directory = root / "published-before-cleanup";
      auto data = sample(8192);
      std::string id;
      fs::path previous_file;
      {
        Store store(directory, {}, true);
        id = ingest(store, data);
        auto keys = store.objects();
        std::map<ObjectKey, Bytes> envelopes;
        for (const auto &key : keys)
          envelopes.emplace(key, read_bytes(loose(store, key)));
        store.pack();
        previous_file = pack_file(directory, ".syp");
        auto next =
            PackStorage::build(directory, keys, [&](const ObjectKey &key) { return envelopes.at(key); });
        auto document = config(directory);
        document["pack"] = next.metadata();
        atomic_write(directory / "config.json", bytes(document.dump()), true);
        // Simulate interruption immediately after the publication pointer changed.
        for (const auto &[key, envelope] : envelopes)
          atomic_write(loose(store, key), envelope, false);
      }
      Store store(directory);
      check(store.objects().size() == 3 && store.fsck() == 3);
      restore(store, id, data);
      store.pack();
      check(!fs::exists(previous_file) && store.fsck() == 3);
    });
    test("failed pack source does not publish a generation or remove its predecessors", [&] {
      auto directory = root / "failed-build";
      Store store(directory, {}, true);
      auto id = ingest(store, sample(8192));
      auto before = read_bytes(directory / "config.json");
      auto keys = store.objects();
      std::size_t reads = 0;
      fails([&] {
        (void)PackStorage::build(directory, keys, [&](const ObjectKey &key) {
          if (++reads == 2)
            throw Error("injected source failure");
          return read_bytes(loose(store, key));
        });
      });
      check(read_bytes(directory / "config.json") == before && store.fsck() == 3);
      for (const auto &file : fs::directory_iterator(directory / "packs")) {
        (void)file;
        throw Error("failed streaming writer left a file");
      }
      restore(store, id, sample(8192));
    });
    test("native archives protect packed history and export after detaching and GC", [&] {
      auto project = root / "project";
      fs::create_directory(project);
      atomic_write(project / "asset.bin", bytes("first"), false);
      Store store(project / ".syrnck", {}, true);
      auto first = store.commit_directory(project, "main", "first");
      atomic_write(project / "asset.bin", bytes("second"), true);
      auto second = store.commit_directory(project, "main", "second");
      store.pack();
      auto detached = store.detach_history("main", "detach");
      check(detached != second && store.gc(true)["unreachable_objects"] == 0);
      auto archive = store.refs();
      for (const auto &[name, key] : archive)
        if (name.starts_with("archive/")) {
          check(key.id == second);
          store.remove_ref(name);
        }
      check(store.gc(true)["unreachable_objects"].get<std::size_t>() > 0);
      store.export_revision(detached, root / "exported");
      check(text(read_bytes(root / "exported" / "asset.bin")) == "second");
      fails([&] { store.get({Kind::revision, first}); });
      check(store.fsck() == 3);
    });
    test("empty stores and empty files pack and collect cleanly", [&] {
      Store store(root / "empty", {}, true);
      check(store.pack()["objects"] == 0 && config(store.root())["pack"].is_null());
      auto id = ingest(store, "");
      check(store.pack()["objects"] == 1);
      restore(store, id, "");
      store.remove_ref("files/" + id);
      store.gc(true);
      check(store.objects().empty() && store.fsck() == 0);
    });
    test("pack metadata cannot select paths outside its generated namespace", [&] {
      auto directory = root / "bad-metadata";
      {
        Store store(directory, {}, true);
        ingest(store, "data");
        store.pack();
      }
      auto original = config(directory);
      for (const auto &bad_name : {"../outside", "UPPERCASE", "", "."}) {
        auto changed = original;
        changed["pack"]["name"] = bad_name;
        atomic_write(directory / "config.json", bytes(changed.dump()), true);
        fails([&] { Store store(directory); });
      }
      auto changed = original;
      changed["pack"]["objects"] = PackStorage::max_entries + 1;
      atomic_write(directory / "config.json", bytes(changed.dump()), true);
      fails([&] { Store store(directory); });
      atomic_write(directory / "config.json", bytes(original.dump()), true);
      Store store(directory);
      check(store.fsck() == 2);
    });
    test("maintenance rejects symlinked loose directories before deleting source files", [&] {
      for (bool kind_directory : {false, true}) {
        auto directory = root / (kind_directory ? "linked-kind" : "linked-objects");
        Store store(directory, {}, true);
        ingest(store, "must remain intact");
        auto link = directory / "objects";
        if (kind_directory)
          link /= "chunk";
        auto target = root / (kind_directory ? "outside-kind" : "outside-objects");
        fs::rename(link, target);
        std::error_code error;
        fs::create_directory_symlink(target, link, error);
        if (error) {
          fs::rename(target, link);
          std::cout << "SKIP symlink checks: " << error.message() << '\n';
          return;
        }
        auto before = config(directory);
        fails([&] { store.fsck(); });
        fails([&] { store.pack(); });
        fails([&] { store.gc(true); });
        check(config(directory) == before);
        bool retained = false;
        for (const auto &file : fs::recursive_directory_iterator(target))
          retained = retained || file.is_regular_file();
        check(retained, "maintenance removed a symlink target");
        fs::remove(link);
        fs::rename(target, link);
        check(store.fsck() == 2);
      }
    });
    std::cout << count << " pack scenarios passed\n";
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
