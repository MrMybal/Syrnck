#include "native_history_fixture.hpp"
#include <iostream>
using namespace native_test;
namespace {
void exercise(const fs::path &root, std::optional<Key> key, unsigned layout, ProgressPhase phase,
              unsigned action) {
  Fixture f(root, key);
  const ObjectKey old{Kind::manifest, f.file("old file")["manifest"].get<std::string>()};
  const ObjectKey next{Kind::manifest, f.file("new file")["manifest"].get<std::string>()};
  f.store.set_ref("left", old);
  f.store.set_ref("right", old);
  f.store.set_ref("unrelated", next);
  f.pack(layout);
  const auto before = f.store.refs();
  bool changed = false;
  const bool applied = f.store.compare_update_refs({{"left", old, next}, {"right", old, {}}},
                                                   {{}, [&](const ProgressEvent &event) {
                                                      if (changed || event.phase != phase)
                                                        return;
                                                      changed = true;
                                                      if (action == 0)
                                                        f.store.set_ref("observer/new", old);
                                                      else if (action == 1)
                                                        f.store.set_ref("left", next);
                                                      else if (action == 2)
                                                        f.store.remove_ref("left");
                                                      else
                                                        f.store.set_ref("unrelated", next);
                                                    }});
  require(changed, "reference observer fence fixture did not reach callback");
  auto expected = before;
  if (action == 0)
    expected["observer/new"] = old;
  else if (action == 1)
    expected["left"] = next;
  else if (action == 2)
    expected.erase("left");
  else {
    expected["left"] = next;
    expected.erase("right");
  }
  require(applied == (action == 3) && f.store.refs() == expected,
          "reference publication overwrote observer updates or split transaction");
  if (action != 3) {
    const auto actual = f.store.refs();
    const auto left = actual.find("left");
    require(f.store.compare_update_refs(
                {{"left", left == actual.end() ? std::optional<ObjectKey>{} : left->second, next},
                 {"right", old, {}}}),
            "fresh reference retry failed");
  }
  f.store.gc(true);
  require(f.store.fsck() > 0, "observer fence lost retained payloads");
}
} // namespace
int main() {
  Temporary temporary;
  try {
    unsigned count = 0;
    for (bool encrypted : {false, true})
      for (unsigned layout = 0; layout < 3; ++layout)
        for (auto phase : {ProgressPhase::verifying, ProgressPhase::publishing})
          for (unsigned action = 0; action < 4; ++action) {
            std::optional<Key> key;
            if (encrypted) {
              key.emplace();
              key->fill(80);
            }
            exercise(temporary.path / std::to_string(count++), key, layout, phase, action);
          }
    std::cout << count << " reference observer fence scenarios passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
