#include "internal.hpp"
#include "native_graph.hpp"
#include <algorithm>
#include <iostream>
#include <source_location>
#include <sstream>
using namespace syrnck;
namespace {
template <class F> void fails(F function, std::source_location location = std::source_location::current()) {
  bool failed = false;
  try {
    function();
  } catch (const Error &) {
    failed = true;
  }
  require(failed, "expected native graph failure at line " + std::to_string(location.line()));
}
std::string revision(const Store &store, std::string_view message, Json parents = Json::array(),
                     Json files = Json::object()) {
  return store
      .put(Kind::revision,
           bytes(Json{
               {"version", 1}, {"parents", parents}, {"message", message}, {"timestamp", 0}, {"files", files}}
                     .dump()))
      .first.id;
}
void scenario(const fs::path &root, std::optional<Key> key, unsigned layout) {
  Store store(root, key, true);
  const auto base = revision(store, "base");
  const auto a = revision(store, "left", Json::array({base}));
  const auto b = revision(store, "right", Json::array({base}));
  const auto left = revision(store, "left merge", Json::array({a, b}));
  const auto right = revision(store, "right merge", Json::array({b, a}));
  const auto other = revision(store, "unrelated");
  const auto tip = revision(store, "tip", Json::array({left, right, right}));
  store.set_ref("heads/main", {Kind::revision, tip});
  if (layout == 1)
    store.pack();
  if (layout == 2)
    store.pack({512 * 1024 * 1024, 2});
  const auto refs = store.refs();
  const auto objects = store.objects();
  auto report = store.native_ancestry(left, right);
  std::vector<std::string> expected{a, b};
  std::sort(expected.begin(), expected.end());
  require(report["merge_bases"] == expected && report["related"] == true &&
              report["source_is_ancestor"] == false && report["target_is_ancestor"] == false &&
              report["visited_revisions"] == 5 && report["parent_links"] == 6,
          "criss-cross bases or traversal counts differ");
  const NativeAncestryGraph loaded(store, {tip, other}, {}, {});
  std::size_t cost = 0;
  const auto grouped = loaded.compare({a, b}, {right}, {}, [&](auto n) { cost += n; });
  require(grouped.bases == expected && grouped.source_is_ancestor && !grouped.target_is_ancestor &&
              cost == loaded.size() + loaded.parent_links(),
          "virtual-parent ancestry or shared work counts differ");
  require(loaded.compare({left}, {right}, {}).bases == expected,
          "reused graph lost singleton ancestry");
  fails([&] { loaded.compare({std::string(64, '0')}, {right}, {}); });
  fails([&] { loaded.compare({left}, {right}, {}, [](auto) { throw Error("work limit"); }); });
  fails([&] { NativeAncestryGraph empty(store, {}, {}, {}); });
  report = store.native_ancestry(base, tip);
  require(report["source_is_ancestor"] == true && report["target_is_ancestor"] == false &&
              report["merge_bases"] == Json::array({base}) && report["parent_links"] == 9,
          "ancestor detection ignored merge edges or duplicate parents");
  report = store.native_ancestry(tip, base);
  require(report["target_is_ancestor"] == true && report["source_is_ancestor"] == false,
          "ancestor direction reversed");
  report = store.native_ancestry(left, tip);
  require(report["merge_bases"] == Json::array({left}), "non-best common ancestors remained");
  report = store.native_ancestry(tip, tip);
  require(report["source_is_ancestor"] == true && report["target_is_ancestor"] == true &&
              report["merge_bases"] == Json::array({tip}),
          "reflexive ancestry differs");
  report = store.native_ancestry(left, other);
  require(report["related"] == false && report["merge_bases"].empty(), "unrelated histories share a base");
  require(store.native_ancestry(base, base, {1, 0})["visited_revisions"] == 1, "root budget differs");
  fails([&] { store.native_ancestry(left, right, {4, 100}); });
  fails([&] { store.native_ancestry(left, right, {5, 5}); });
  fails([&] { store.native_ancestry(left, right, {0, 0}); });
  fails([&] { store.native_ancestry(left, right, {1000001, 0}); });
  fails([&] { store.native_ancestry(left, right, {5, 10000001}); });
  fails([&] { store.native_ancestry("HEAD", right); });
  const auto missing = revision(store, "missing parent", Json::array({std::string(64, '0')}));
  fails([&] { store.native_ancestry(missing, missing); });
  std::stop_source stop;
  fails([&] {
    store.native_ancestry(left, right, {},
                          {stop.get_token(), [&](const ProgressEvent &) { stop.request_stop(); }});
  });
  require(stop.stop_requested(), "graph did not expose cancellation");
  fails([&] {
    store.native_ancestry(left, right, {},
                          {{}, [](const ProgressEvent &) { throw Error("observer failure"); }});
  });
  require(store.refs() == refs && store.objects().size() == objects.size() + 1,
          "graph inspection changed retention or objects");
}
void long_history(const fs::path &root) {
  Store store(root, {}, true);
  const auto base = revision(store, "root");
  auto tip = base;
  for (unsigned i = 0; i != 1024; ++i)
    tip = revision(store, std::to_string(i), Json::array({tip}));
  const auto report = store.native_ancestry(base, tip, {1025, 1024});
  require(report["visited_revisions"] == 1025 && report["merge_bases"] == Json::array({base}),
          "iterative long-history traversal differs");
}
void cached_queries(const fs::path &root) {
  Store store(root, {}, true);
  const auto a = revision(store, "root"), b = revision(store, "child", Json::array({a}));
  const NativeAncestryGraph graph(store, {b}, {}, {});
  // The immutable graph must answer without touching Store metadata again.
  require(fs::remove(root / "objects/revision" / a.substr(0, 2) / a) &&
              fs::remove(root / "objects/revision" / b.substr(0, 2) / b), "cached graph fixture failed");
  require(graph.compare({a}, {b}, {}).bases == std::vector<std::string>{a} &&
              graph.compare({b}, {b}, {}).bases == std::vector<std::string>{b},
          "reused ancestry graph reread removed metadata");
}
void missing_payload(const fs::path &root) {
  std::string tip, chunk;
  {
    Store store(root, {}, true);
    std::istringstream input("graph traversal never reads file payloads");
    const auto manifest = store.ingest(input).first;
    chunk = store.metadata(Kind::manifest, manifest)["blocks"][0]["id"].get<std::string>();
    tip = revision(store, "files", Json::array(),
                   Json{{"asset.bin", Json{{"manifest", manifest}, {"executable", false}}}});
  }
  require(fs::remove(root / "objects/chunk" / chunk.substr(0, 2) / chunk), "missing payload fixture failed");
  Store store(root);
  require(store.native_ancestry(tip, tip)["merge_bases"] == Json::array({tip}),
          "ancestry required file payloads");
}
} // namespace
int main() {
  const auto root = fs::temp_directory_path() / ("syrnck-native-graph-" + hex(random_bytes(16)));
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code error;
      fs::remove_all(path, error);
    }
  } cleanup{root};
  try {
    for (bool encrypted : {false, true})
      for (unsigned layout = 0; layout != 3; ++layout) {
        std::optional<Key> key;
        if (encrypted) {
          key.emplace();
          key->fill(47);
        }
        scenario(root / (std::to_string(encrypted) + '-' + std::to_string(layout)), key, layout);
      }
    long_history(root / "long");
    missing_payload(root / "missing-payload");
    cached_queries(root / "cached");
    std::cout
        << "native ancestry, best common ancestors, bounded iterative traversal and cancellation passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
