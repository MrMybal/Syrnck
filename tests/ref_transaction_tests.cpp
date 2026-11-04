#include <syrnck/core.hpp>
#include <iostream>
#include <sstream>
using namespace syrnck;
namespace {
template <class F> void fails(F function) {
  bool failed = false;
  try {
    function();
  } catch (const Error &) {
    failed = true;
  }
  require(failed, "expected transaction failure");
}
} // namespace
int main() {
  auto root = fs::temp_directory_path() / ("syrnck-ref-transactions-" + hex(random_bytes(16)));
  struct Cleanup {
    fs::path root;
    ~Cleanup() {
      std::error_code e;
      fs::remove_all(root, e);
    }
  } cleanup{root};
  try {
    for (bool encrypted : {false, true}) {
      std::optional<Key> key;
      if (encrypted) {
        key.emplace();
        key->fill(51);
      }
      Store store(root / std::to_string(encrypted), key, true);
      std::istringstream a("old file"), b("new file");
      ObjectKey old{Kind::manifest, store.ingest(a).first}, next{Kind::manifest, store.ingest(b).first};
      store.set_ref("left", old);
      store.set_ref("right", old);
      store.set_ref("unrelated", next);
      const auto before = store.refs();
      require(!store.compare_update_refs({{"left", old, next}, {"right", next, {}}}),
              "CAS conflict accepted");
      require(store.refs() == before, "CAS conflict changed refs");
      fails([&] { store.compare_update_refs({{"left", old, next}, {"left", old, {}}}); });
      fails([&] {
        store.compare_update_refs(
            {{"left", old, next}, {"missing", {}, ObjectKey{Kind::manifest, std::string(64, '0')}}});
      });
      require(store.refs() == before, "invalid closure changed refs");
      if (!encrypted) {
        std::vector<RefUpdate> oversized;
        for (unsigned i = 0; i != 60000; ++i)
          oversized.push_back({"overflow/" + std::string(180, 'a') + '/' + std::to_string(i), {}, old});
        fails([&] { store.compare_update_refs(oversized); });
        require(store.refs() == before, "oversized transaction changed refs");
      }
      std::stop_source stop;
      fails([&] {
        store.compare_update_refs({{"left", old, next}, {"right", old, {}}},
                                  {stop.get_token(), [&](const ProgressEvent &event) {
                                     if (event.phase == ProgressPhase::publishing)
                                       stop.request_stop();
                                   }});
      });
      require(store.refs() == before, "cancelled transaction changed refs");
      require(store.compare_update_refs({{"left", old, next}, {"right", old, {}}, {"created", {}, old}}),
              "valid transaction rejected");
      auto after = store.refs();
      require(after.at("left") == next && !after.contains("right") && after.at("created") == old &&
                  after.at("unrelated") == next,
              "transaction result differs");
      require(store.compare_update_refs({{"absent", {}, {}}}), "absent no-op rejected");
      require(store.compare_update_refs({}), "empty transaction rejected");
      store.pack();
      store.gc(true);
      require(store.fsck() >= 4, "transaction lost file dependencies");
    }
    std::cout << "Verified atomic reference updates, compare conflicts, complete closures and cancellation\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
