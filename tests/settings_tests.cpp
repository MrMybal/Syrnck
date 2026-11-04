#include "internal.hpp"
#include <iostream>
#include <sstream>
using namespace syrnck;
namespace {
void check(bool value) {
  require(value, "repository settings test failed");
}
template <class F> void fails(F f) {
  bool failed = false;
  try {
    f();
  } catch (const std::exception &) {
    failed = true;
  }
  check(failed);
}
} // namespace
int main() {
  auto root = fs::temp_directory_path() / path_from_utf8("syrnck-settings-" + hex(random_bytes(16)));
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code ec;
      fs::remove_all(path, ec);
    }
  } cleanup{root};
  try {
    auto defaults = RepositorySettings{}.json();
    {
      Store store(root, {}, true);
      check(store.settings().json() == defaults && !fs::exists(root / "settings.json"));
      auto config = read_bytes(root / "config.json"), refs = read_bytes(root / "refs.json");
      RepositorySettings settings{4, 4 * 1024 * 1024, 18 * 1024 * 1024, 2};
      store.set_settings(settings);
      check(store.settings().json() == settings.json());
      check(read_bytes(root / "config.json") == config && read_bytes(root / "refs.json") == refs);
      auto valid = read_bytes(root / "settings.json");
      for (int fault = 0; fault < 7; ++fault) {
        auto bad = settings;
        if (fault == 0)
          bad.ingest_workers = 0;
        if (fault == 1)
          bad.ingest_workers = 65;
        if (fault == 2)
          bad.ingest_buffer_bytes = 0;
        if (fault == 3)
          bad.ingest_buffer_bytes = 1024ULL * 1024 * 1024 + 1;
        if (fault == 4)
          bad.pack_max_bytes = 1;
        if (fault == 5)
          bad.pack_max_objects = 0;
        if (fault == 6)
          bad.pack_max_objects = 1048577;
        fails([&] { store.set_settings(bad); });
        check(read_bytes(root / "settings.json") == valid);
      }
      std::string data;
      for (int i = 0; i < 10; ++i)
        data += std::string(1024, static_cast<char>('a' + i));
      std::istringstream input(data);
      auto [id, stats] = store.ingest(input, Chunking::fixed_size(1024));
      check(stats.peak_in_flight_blocks > 0 && stats.peak_in_flight_bytes <= settings.ingest_buffer_bytes);
      std::istringstream serial(data);
      auto [same, single] = store.ingest(serial, Chunking::fixed_size(1024), {});
      check(same == id && single.peak_in_flight_blocks == 0);
      check(store.pack()["pack_count"] == 6);
      check(store.fsck() == 11);
    }
    Store reopened(root);
    check(reopened.settings().ingest_workers == 4);
    auto valid = reopened.settings().json();
    for (int fault = 0; fault < 10; ++fault) {
      auto bad = valid;
      if (fault == 0)
        bad["version"] = 2;
      if (fault == 1)
        bad["version"] = 1.0;
      if (fault == 2)
        bad["token"] = "not accepted";
      if (fault == 3)
        bad["ingest"]["workers"] = -1;
      if (fault == 4)
        bad["ingest"]["workers"] = true;
      if (fault == 5)
        bad["ingest"]["workers"] = 4.5;
      if (fault == 6)
        bad["pack"]["max_objects"] = UINT64_MAX;
      if (fault == 7)
        bad["pack"]["compact"] = true;
      if (fault == 8)
        bad["ingest"].erase("buffer_bytes");
      if (fault == 9)
        bad = Json::array();
      atomic_write(root / "settings.json", bytes(bad.dump()), true);
      fails([&] { reopened.settings(); });
      check(reopened.fsck() == 11); // Preferences do not change storage readability.
    }
    reopened.set_settings({}); // Explicit reset can replace a malformed regular file.
    check(reopened.settings().json() == defaults);
    fs::remove(root / "settings.json");
    fs::create_directory(root / "settings.json");
    fails([&] { reopened.settings(); });
    fails([&] { reopened.set_settings({}); });
    std::cout << "Repository settings: defaults, persistence, typed validation, overrides and pack budgets "
                 "passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
