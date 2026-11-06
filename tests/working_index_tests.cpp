#include <syrnck/working_index.hpp>
#include "native_workspace.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif
using namespace syrnck;
namespace {
void write(const fs::path &path, std::string_view value) {
  fs::create_directories(path.parent_path());
  atomic_write(path, bytes(value), true);
}
template <class F> void fails(F f) {
  bool failed = false;
  try { f(); } catch (const Error &) { failed = true; }
  require(failed, "expected working index failure");
}
void equivalent(const Json &indexed, const Json &full) {
  auto report = indexed;
  report.erase("working_index");
  report["hashed_bytes"] = full["hashed_bytes"];
  require(report == full, "indexed status differs from full byte comparison");
}
void scenario(const fs::path &project, std::optional<Key> key, unsigned layout) {
  const auto large = std::string(1024 * 1024 + 7, 'a');
  std::string baseline;
  std::map<std::string, ObjectKey> references;
  {
    Store store(project / ".syrnck", key, true);
    write(project / "asset.bin", large);
    write(project / "nested/source.cpp", "int main() { return 0; }\n");
    baseline = store.commit_directory(project, "main", "baseline");
    references = store.refs();
    const auto full = store.status_directory(project, baseline);
    const auto first = indexed_native_status(store, project, baseline);
    equivalent(first, full);
    require(first["working_index"]["previous"] == "missing" && first["working_index"]["reused_files"] == 0,
            "first indexed scan reused unverified contents");
    require(first["hashed_bytes"] == full["hashed_bytes"], "first indexed scan skipped file bytes");
    if (layout == 1) store.pack();
    if (layout == 2) store.pack({512 * 1024 * 1024, 2});
  }
  // Cross-process persistence cannot depend on Store's decoded object cache.
  std::this_thread::sleep_for(std::chrono::milliseconds(3100));
  Store store(project / ".syrnck", key);
  const auto second = indexed_native_status(store, project, baseline);
  equivalent(second, store.status_directory(project, baseline));
  require(second["working_index"]["previous"] == "ready", "persisted index was not reopened");
  const bool supported = second["working_index"]["supported_files"] == second["workspace_files"];
  if (supported)
    require(second["hashed_bytes"] == 0 && second["working_index"]["reused_files"] == 2,
            "unchanged supported files were rehashed");
#ifdef _WIN32
  {
    const auto source = project / "nested/source.cpp";
    const auto handle = CreateFileW(source.c_str(), GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(handle != INVALID_HANDLE_VALUE, "cannot open active writer fixture");
    struct Close { HANDLE handle; ~Close() { CloseHandle(handle); } } close{handle};
    const auto active = indexed_native_status(store, project, baseline);
    require(active["hashed_bytes"].get<std::uint64_t>() >= fs::file_size(source),
            "active Windows writer reused a potentially deferred change timestamp");
    equivalent(active, store.status_directory(project, baseline));
  }
#else
  fs::permissions(project / "nested/source.cpp", fs::perms::owner_exec, fs::perm_options::add);
  const auto mode = indexed_native_status(store, project, baseline);
  equivalent(mode, store.status_directory(project, baseline));
  require(!mode["clean"].get<bool>(), "cached executable change was lost");
  fs::permissions(project / "nested/source.cpp", fs::perms::owner_exec, fs::perm_options::remove);
#endif

  const auto asset = project / "asset.bin";
  const auto original_time = fs::last_write_time(asset);
  {
    std::ofstream out(asset, std::ios::binary | std::ios::trunc);
    out << std::string(large.size(), 'b');
    require(out.good(), "cannot change working asset");
  }
  fs::last_write_time(asset, original_time);
  auto changed = indexed_native_status(store, project, baseline);
  equivalent(changed, store.status_directory(project, baseline));
  require(!changed["clean"].get<bool>() && changed["hashed_bytes"].get<std::uint64_t>() >= large.size(),
          "same-size/same-time in-place edit was hidden by the index");
  auto immediate = indexed_native_status(store, project, baseline);
  require(immediate["hashed_bytes"].get<std::uint64_t>() >= large.size(), "recent change time was reused");

  write(asset, large);
  fs::last_write_time(asset, original_time);
  equivalent(indexed_native_status(store, project, baseline), store.status_directory(project, baseline));
  fs::rename(project / "nested/source.cpp", project / "nested/renamed.cpp");
  const auto renamed = indexed_native_status(store, project, baseline);
  equivalent(renamed, store.status_directory(project, baseline));
  require(renamed["counts"]["untracked"] == 1 && renamed["counts"]["removed"] == 1,
          "cached rename was not inventoried");
  write(project / ".syrnckignore", "nested/\n");
  equivalent(indexed_native_status(store, project, baseline), store.status_directory(project, baseline));
  fs::remove(project / ".syrnckignore");

  const auto staged = store.stage_directory(project, "main", {"asset.bin"});
  write(asset, "later edit");
  auto combined = indexed_native_workspace_status(store, project, "main");
  auto regular_combined = store.workspace_status(project, "main");
  equivalent(combined["unstaged"], regular_combined["unstaged"]);
  require(combined["staged"] == regular_combined["staged"] && !combined["clean"].get<bool>() &&
              combined["staged"]["snapshot"] == staged, "combined indexed staging changed meaning");
  store.reset_stage("main");
  require(store.refs() == references, "working index altered retained roots or staging");

  const auto pointer = store.root() / "working-index.json";
  write(pointer, "{}");
  const auto invalid = indexed_native_status(store, project, baseline);
  require(invalid["working_index"]["previous"] == "invalid" && invalid["working_index"]["reused_files"] == 0,
          "malformed pointer was trusted");
  const auto object = parse_json(read_bytes(pointer))["object"].get<std::string>();
  const auto cache_path = store.root() / "objects/chunk" / object.substr(0, 2) / object;
  auto envelope = read_bytes(cache_path);
  envelope[0] ^= 1;
  atomic_write(cache_path, envelope, true);
  const auto corrupt = indexed_native_status(store, project, baseline);
  require(corrupt["working_index"]["previous"] == "invalid" && corrupt["working_index"]["reused_files"] == 0,
          "corrupted cached envelope was reused");
  equivalent(corrupt, store.status_directory(project, baseline));
  store.gc(true);
  const auto collected = indexed_native_status(store, project, baseline);
  require(collected["working_index"]["previous"] == "invalid" && collected["working_index"]["reused_files"] == 0,
          "collected cache chunks were assumed durable");
  equivalent(collected, store.status_directory(project, baseline));

  const auto before = read_bytes(pointer);
  const auto objects = store.objects();
  for (bool clear : {false, true}) {
    std::stop_source stop;
    bool seen = false;
    OperationOptions options{stop.get_token(), [&](const ProgressEvent &event) {
      if (event.phase == ProgressPhase::publishing) { seen = true; stop.request_stop(); }
    }};
    fails([&] {
      if (clear) clear_native_working_index(store, options);
      else indexed_native_status(store, project, baseline, options);
    });
    require(seen && read_bytes(pointer) == before && store.objects() == objects,
            "cancelled cache publication changed the Store");
  }
  bool changed_pointer = false;
  fails([&] {
    indexed_native_status(store, project, baseline, {{}, [&](const ProgressEvent &event) {
      if (event.phase == ProgressPhase::publishing) { write(pointer, "{}"); changed_pointer = true; }
    }});
  });
  require(changed_pointer && text(read_bytes(pointer)) == "{}" && store.objects() == objects,
          "observer cache-pointer change was overwritten");
  indexed_native_status(store, project, baseline);
  require(clear_native_working_index(store)["state"] == "cleared" && !fs::exists(pointer),
          "working index was not cleared");
  require(clear_native_working_index(store)["state"] == "absent", "empty cache clear was not idempotent");
  fs::create_directory(pointer);
  fails([&] { indexed_native_status(store, project, baseline); });
  fails([&] { clear_native_working_index(store); });
  require(fs::is_directory(pointer), "invalid pointer obstruction was removed");
  fs::remove(pointer);
  require(store.refs() == references, "cache maintenance altered references");
}
} // namespace
int main() {
  const auto root = fs::temp_directory_path() / ("syrnck-working-index-" + hex(random_bytes(16)));
  struct Cleanup {
    fs::path path;
    ~Cleanup() { std::error_code error; fs::remove_all(path, error); }
  } cleanup{root};
  try {
    for (bool encrypted : {false, true})
      for (unsigned layout = 0; layout != 3; ++layout) {
        std::optional<Key> key;
        if (encrypted) { key.emplace(); key->fill(71); }
        scenario(root / (std::to_string(encrypted) + "-" + std::to_string(layout)), key, layout);
      }
    std::cout << "persistent working index: six storage/encryption layouts passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
