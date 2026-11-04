#include "internal.hpp"
#include "pack_storage.hpp"
#include <iostream>
#include <sstream>
using namespace syrnck;
namespace {
std::stop_source *checkpoint_stop = nullptr;
std::string checkpoint_phase;
bool checkpoint_fired = false;
void check(bool condition) {
  require(condition, "maintenance cancellation test failed");
}
template <class F> void cancelled(F function) {
  bool seen = false;
  try {
    function();
  } catch (const Cancelled &) {
    seen = true;
  }
  check(seen);
}
ObjectKey ingest(const Store &store, std::string data) {
  std::istringstream input(data);
  return {Kind::manifest, store.ingest(input, Chunking::fixed_size(1024)).first};
}
std::string content(std::size_t first) {
  std::string result;
  for (std::size_t i = first; i < first + 12; ++i)
    result += std::string(1024, static_cast<char>(i));
  return std::string(1024, 's') + result;
}
fs::path loose(const Store &store, const ObjectKey &key) {
  return store.root() / "objects" / kind_name(key.kind) / key.id.substr(0, 2) / key.id;
}
std::map<fs::path, Bytes> source_files(const Store &store) {
  std::map<fs::path, Bytes> result;
  for (const auto &name : {"refs.json", "config.json"})
    result.emplace(store.root() / name, read_bytes(store.root() / name));
  for (const auto &key : store.objects())
    if (fs::exists(loose(store, key)))
      result.emplace(loose(store, key), read_bytes(loose(store, key)));
  auto config = parse_json(read_bytes(store.root() / "config.json"));
  auto catalog = config["version"] == 3 ? config["packs"] : Json::array();
  if (config["version"] == 2 && !config["pack"].is_null())
    catalog.push_back(config["pack"]);
  for (const auto &pack : catalog)
    for (const auto &extension : {".syp", ".syi"}) {
      auto path =
          store.root() / "packs" / path_from_utf8("pack-" + pack["name"].get<std::string>() + extension);
      result.emplace(path, read_bytes(path));
    }
  return result;
}
void no_temporaries(const Store &store) {
  for (const auto &entry : fs::recursive_directory_iterator(store.root()))
    check(!utf8(entry.path().filename()).starts_with(".tmp"));
}
void retained(const Store &store, const ObjectKey &key) {
  std::ostringstream output;
  store.reconstruct(key.id, output);
  check(output.str() == content(1));
}
} // namespace
namespace syrnck::testing {
void io_checkpoint(std::string_view phase, const fs::path &target) {
  if (checkpoint_stop && phase == checkpoint_phase &&
      (phase != "atomic.published" || target.filename() == "config.json")) {
    checkpoint_fired = true;
    checkpoint_stop->request_stop();
  }
}
} // namespace syrnck::testing
int main() {
  auto root = fs::temp_directory_path() / path_from_utf8("syrnck-maintenance-" + hex(random_bytes(16)));
  fs::create_directory(root);
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code error;
      fs::remove_all(path, error);
    }
  } cleanup{root};
  try {
    for (bool encrypted : {false, true})
      for (bool compact : {false, true}) {
        std::optional<Key> key;
        if (encrypted) {
          key.emplace();
          key->fill(0x51);
        }
        Store store(root / (std::to_string(encrypted) + "-" + std::to_string(compact)), key, true);
        auto live = ingest(store, content(1));
        store.set_ref("retained", live);
        ingest(store, content(30));
        store.pack();               // One partially reachable pack.
        ingest(store, content(60)); // Also exercise loose metadata and blocks.
        auto before = source_files(store);
        auto inventory = store.objects();
        auto refs = store.refs();
        auto run = [&](OperationOptions operation) {
          return compact ? store.pack({.max_objects_per_pack = 3, .compact = true}, operation)
                         : store.gc(true, operation);
        };
        std::stop_source pre;
        pre.request_stop();
        cancelled([&] { run({pre.get_token(), {}}); });
        for (int rule = 0; rule < 5; ++rule) {
          std::stop_source stop;
          bool packing_seen = false;
          cancelled([&] {
            run({stop.get_token(), [&](const ProgressEvent &event) {
                   packing_seen |= event.phase == ProgressPhase::packing;
                   bool matches = (rule == 0 && event.phase == ProgressPhase::verifying && event.bytes > 0) ||
                                  (rule == 1 && event.phase == ProgressPhase::planning && event.bytes > 0) ||
                                  (rule == 2 && event.phase == ProgressPhase::packing && event.bytes > 0) ||
                                  (rule == 3 && event.phase == ProgressPhase::verifying && packing_seen &&
                                   event.bytes > 0) ||
                                  (rule == 4 && event.phase == ProgressPhase::publishing);
                   if (matches)
                     stop.request_stop();
                 }});
          });
          for (const auto &[path, data] : before)
            check(read_bytes(path) == data);
          check(store.refs() == refs && store.objects() == inventory && store.fsck() == inventory.size());
          retained(store, live);
          no_temporaries(store);
        }
        bool observer_error = false;
        try {
          run({{}, [](const ProgressEvent &event) {
                 if (event.phase == ProgressPhase::packing && event.bytes > 0)
                   throw std::logic_error("maintenance observer sentinel");
               }});
        } catch (const std::logic_error &error) {
          observer_error = std::string_view(error.what()) == "maintenance observer sentinel";
        }
        check(observer_error && store.fsck() == inventory.size());
        std::stop_source late;
        checkpoint_stop = &late;
        checkpoint_phase = "atomic.published";
        checkpoint_fired = false;
        auto result = run({late.get_token(), {}});
        checkpoint_stop = nullptr;
        check(checkpoint_fired && late.stop_requested());
        check(compact ? result["compacted"] == true : result["deleted"] == true);
        retained(store, live);
        check(store.refs() == refs && store.fsck() > 0);
        if (!compact)
          check(store.gc()["unreachable_objects"] == 0);
        else
          check(store.objects() == inventory);
        no_temporaries(store);
      }
    // No catalog update: GC's first deletion is the commit point.
    Store loose_only(root / "loose-only", {}, true);
    auto live = ingest(loose_only, content(1));
    loose_only.set_ref("retained", live);
    ingest(loose_only, content(30));
    ingest(loose_only, content(60));
    std::stop_source late;
    checkpoint_stop = &late;
    checkpoint_phase = "gc.loose_removed";
    checkpoint_fired = false;
    check(loose_only.gc(true, {late.get_token(), {}})["deleted"] == true);
    checkpoint_stop = nullptr;
    check(checkpoint_fired && loose_only.gc()["unreachable_objects"] == 0);
    retained(loose_only, live);
    check(loose_only.fsck() > 0);
    std::cout << "Maintenance preparation cancellation and late-stop cleanup passed\n";
  } catch (const std::exception &error) {
    checkpoint_stop = nullptr;
    std::cerr << error.what() << '\n';
    return 1;
  }
}
