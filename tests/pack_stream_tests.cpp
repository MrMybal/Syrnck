#include "pack_index_fixture.hpp"
#include <iostream>
#include <sstream>
using namespace syrnck;
namespace fixture = index_fixture;
namespace {
void check(bool value) {
  require(value, "streaming pack test failed");
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
fs::path loose(const Store &store, const ObjectKey &key) {
  return store.root() / "objects" / kind_name(key.kind) / key.id.substr(0, 2) / key.id;
}
std::map<fs::path, Bytes> files(const fs::path &root) {
  std::map<fs::path, Bytes> result;
  for (const auto &file : fs::recursive_directory_iterator(root))
    if (file.is_regular_file())
      result.emplace(file.path(), read_bytes(file.path()));
  return result;
}
PackStorage::KeySequence sequence(const std::vector<ObjectKey> &keys) {
  return [&keys]() -> PackStorage::KeyNext {
    return [&keys, i = std::size_t{0}]() mutable -> std::optional<ObjectKey> {
      return i < keys.size() ? std::optional(keys[i++]) : std::nullopt;
    };
  };
}
} // namespace
int main() {
  auto root = fs::temp_directory_path() / path_from_utf8("syrnck-pack-stream-" + hex(random_bytes(16)));
  fs::create_directory(root);
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code ec;
      fs::remove_all(path, ec);
    }
  } cleanup{root};
  std::size_t count = 0;
  auto test = [&](std::string_view name, auto function) {
    function();
    ++count;
    std::cout << "PASS " << name << '\n';
  };
  try {
    test("two-pass writer produces exact indexes and preserves envelopes across output boundaries", [&] {
      auto directory = root / "writer";
      auto a = fixture::create(directory, 513, 0, 2);
      auto b = fixture::create(directory, 257, 1, 2);
      PackStorage input(directory, 0);
      input.load(Json::array({a, b}));
      auto expected = input.keys();
      std::size_t passes = 0, planned = 0, read = 0;
      auto output = PackStorage::build_many_stream(
          directory, expected.size(),
          [&] {
            ++passes;
            return input.ordered_keys();
          },
          [&](const ObjectKey &key) {
            ++read;
            return input.read(key);
          },
          [&](const ObjectKey &key) {
            ++planned;
            return input.stored_size(key);
          },
          {.max_objects_per_pack = 256}, PackStorage::max_packs, 0);
      check(passes == 2 && planned == expected.size() && read == expected.size());
      check(output.pack_count() == 4 && output.keys() == expected);
      for (std::size_t i = 0; i < 4; ++i) {
        auto metadata = output.catalog()[i];
        auto begin = i * 256, end = std::min(begin + 256, expected.size());
        std::vector<ObjectKey> part(expected.begin() + begin, expected.begin() + end);
        auto oracle =
            PackStorage::build(directory, part, [&](const ObjectKey &key) { return input.read(key); });
        check(read_bytes(fixture::path(directory, metadata)) ==
              read_bytes(fixture::path(directory, oracle.metadata())));
        check(metadata["bytes"] == 8 + 5 * part.size());
      }
      check(input.open_index_streams() == 0 && input.open_streams() == 0);
    });
    test("planning failures create no output and read no payloads", [&] {
      Store store(root / "preflight", {}, true);
      std::vector<ObjectKey> keys{fixture::key(0).object(), fixture::key(1).object()};
      auto before = files(store.root());
      bool read = false;
      auto reader = [&](const ObjectKey &) {
        read = true;
        return Bytes(5);
      };
      for (std::size_t advertised : {0, 1, 3})
        fails([&] {
          PackStorage::build_many_stream(store.root(), advertised, sequence(keys), reader,
                                         [](const auto &) { return 5; }, {});
        });
      fails([&] {
        PackStorage::build_many_stream(
            store.root(), 2, sequence(keys), reader, [](const auto &) { return 5; },
            {.max_objects_per_pack = 1}, 1);
      });
      fails([&] {
        PackStorage::build_many_stream(store.root(), 2, sequence(keys), reader,
                                       [](const auto &) { return 4; }, {});
      });
      std::reverse(keys.begin(), keys.end());
      fails([&] {
        PackStorage::build_many_stream(store.root(), 2, sequence(keys), reader,
                                       [](const auto &) { return 5; }, {});
      });
      check(!read && files(store.root()) == before);
      std::vector<ObjectKey> empty;
      auto result = PackStorage::build_many_stream(
          store.root(), 0, sequence(empty), reader, [](const auto &) { return 5; }, {}, 0);
      check(result.pack_count() == 0 && files(store.root()) == before);
    });
    test("changed counts, IDs, lengths and second-pass failures never publish or remove sources", [&] {
      for (int fault = 0; fault < 6; ++fault) {
        Store store(root / ("changed-" + std::to_string(fault)), {}, true);
        std::vector<ObjectKey> keys{fixture::key(0).object(), fixture::key(2).object()};
        std::size_t passes = 0, read = 0;
        auto before = files(store.root());
        fails([&] {
          PackStorage::build_many_stream(
              store.root(), 2,
              [&]() -> PackStorage::KeyNext {
                auto changed = keys;
                if (++passes == 2) {
                  if (fault == 0)
                    changed.pop_back();
                  if (fault == 1)
                    changed.push_back(fixture::key(3).object());
                  if (fault == 2)
                    changed[1] = fixture::key(1).object();
                }
                return
                    [changed = std::move(changed), i = std::size_t{0}]() mutable -> std::optional<ObjectKey> {
                      return i < changed.size() ? std::optional(changed[i++]) : std::nullopt;
                    };
              },
              [&](const ObjectKey &) {
                if (++read == 2 && fault == 5)
                  throw Error("injected second-pack failure");
                return Bytes(fault == 3 ? 6 : 5);
              },
              [&](const ObjectKey &) {
                if (fault == 4)
                  throw Error("injected planner failure");
                return 5;
              },
              {.max_objects_per_pack = 1});
        });
        for (const auto &[path, bytes] : before)
          check(read_bytes(path) == bytes);
        check(store.fsck() == 0); // Inactive partial outputs do not join the store.
        store.gc(true);
        check(files(store.root()) == before);
      }
    });
    test("mixed compaction retains every object and exact encrypted envelope", [&] {
      for (bool encrypted : {false, true}) {
        std::optional<Key> secret;
        if (encrypted) {
          secret.emplace();
          secret->fill(83);
        }
        Store store(root / (encrypted ? "encrypted" : "plain"), secret, true,
                    {.decoded_cache_bytes = 0, .verified_cache_entries = 0, .pack_index_cache_bytes = 0});
        std::map<ObjectKey, Bytes> envelopes;
        for (std::size_t group = 0; group < 3; ++group) {
          std::string data;
          for (std::size_t i = 0; i < 130; ++i) {
            auto block = "block " + std::to_string(group * 130 + i);
            block.resize(32, '_');
            data += block;
          }
          std::istringstream source(data);
          auto id = store.ingest(source, Chunking::fixed_size(32)).first;
          if (group == 0)
            store.set_ref("retained", {Kind::manifest, id});
          for (const auto &key : store.objects())
            if (fs::exists(loose(store, key)))
              envelopes[key] = read_bytes(loose(store, key));
          if (group != 2)
            store.pack({.max_objects_per_pack = 67});
        }
        // A different valid encrypted envelope for a packed duplicate must win.
        auto duplicate = std::find_if(envelopes.begin(), envelopes.end(), [&](const auto &entry) {
                           return !fs::exists(loose(store, entry.first));
                         })->first;
        auto data = store.get(duplicate);
        Store other(root / (encrypted ? "other-encrypted" : "other-plain"), secret, true);
        check(other.put(duplicate.kind, data).first == duplicate);
        envelopes[duplicate] = read_bytes(loose(other, duplicate));
        atomic_write(loose(store, duplicate), envelopes[duplicate], false);
        auto before = store.objects();
        auto refs = store.refs();
        auto result = store.pack({.max_objects_per_pack = 128, .compact = true});
        check(result["objects_written"] == before.size() && result["pack_count"] == 4);
        check(result["loose_objects_removed"] == 132 && store.refs() == refs);
        check(store.objects() == before && store.fsck() == before.size());
        auto config = parse_json(read_bytes(store.root() / "config.json"));
        PackStorage output(store.root(), 0);
        output.load(config["packs"]);
        for (const auto &[key, envelope] : envelopes)
          check(output.read(key) == envelope);
        auto unchanged = files(store.root());
        check(store.pack()["objects_written"] == 0 && files(store.root()) == unchanged);
      }
    });
    std::cout << count << " streaming pack scenarios passed\n";
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
