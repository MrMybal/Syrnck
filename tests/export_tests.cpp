#include "internal.hpp"
#include <fstream>
#include <iostream>
using namespace syrnck;
namespace {
void check(bool condition) {
  require(condition, "transactional export test failed");
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
template <class F> void fails(F function) {
  bool seen = false;
  try {
    function();
  } catch (const std::exception &) {
    seen = true;
  }
  check(seen);
}
void no_staging(const fs::path &parent) {
  for (const auto &entry : fs::directory_iterator(parent))
    check(!utf8(entry.path().filename()).starts_with(".syrnck-export-"));
}
} // namespace
int main() {
  auto root = fs::temp_directory_path() / path_from_utf8("syrnck-export-test-" + hex(random_bytes(16)));
  fs::create_directory(root);
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code error;
      fs::remove_all(path, error);
    }
  } cleanup{root};
  try {
    std::string data(5 * 1024 * 1024 + 17, 'a');
    for (std::size_t i = 0; i < data.size(); ++i)
      data[i] = static_cast<char>(i * 17);
    for (bool encrypted : {false, true})
      for (bool packed : {false, true}) {
        auto parent = root / (std::to_string(encrypted) + "-" + std::to_string(packed));
        auto project = parent / "project";
        fs::create_directories(project / "nested");
        std::optional<Key> key;
        if (encrypted) {
          key.emplace();
          key->fill(0x17);
        }
        Store store(project / ".syrnck", key, true);
        atomic_write(project / "nested/asset.bin", bytes(data), false);
        atomic_write(project / "script", bytes("#!/bin/sh\nexit 0\n"), false);
        atomic_write(project / "empty", {}, false);
        make_executable(project / "script");
        auto revision = store.commit_directory(project, "main", "Export fixture");
        if (packed)
          store.pack({.max_objects_per_pack = 2});
        auto before = store.refs();
        std::stop_source stopped;
        stopped.request_stop();
        auto dest = parent / "cancelled";
        cancelled([&] { store.export_revision(revision, dest, {stopped.get_token(), {}}); });
        check(!fs::exists(dest));
        for (auto phase : {ProgressPhase::verifying, ProgressPhase::exporting, ProgressPhase::publishing}) {
          std::stop_source stop;
          cancelled([&] {
            store.export_revision(revision, dest,
                                  {stop.get_token(), [&](const ProgressEvent &event) {
                                     if (event.phase == phase &&
                                         (phase == ProgressPhase::publishing || event.bytes > 0))
                                       stop.request_stop();
                                   }});
          });
          check(!fs::exists(dest) && store.refs() == before);
          no_staging(parent);
        }
        bool reported = false;
        try {
          store.export_revision(revision, dest,
                                {{}, [](const ProgressEvent &event) {
                                   if (event.phase == ProgressPhase::exporting && event.bytes > 0)
                                     throw Error("export observer failed");
                                 }});
        } catch (const Error &error) {
          reported = std::string(error.what()) == "export observer failed";
        }
        check(reported && !fs::exists(dest));
        no_staging(parent);
        // Create an empty directory at the last callback. Exclusive rename must
        // refuse it even on POSIX, where ordinary rename would replace it.
        bool collided = false;
        fails([&] {
          store.export_revision(revision, dest, {{}, [&](const ProgressEvent &event) {
                                                   if (event.phase == ProgressPhase::publishing) {
                                                     check(fs::create_directory(dest));
                                                     collided = true;
                                                   }
                                                 }});
        });
        check(collided && fs::is_directory(dest) && fs::is_empty(dest));
        no_staging(parent);
        fs::remove(dest);
        atomic_write(dest, bytes("preserve existing file"), false);
        fails([&] { store.export_revision(revision, dest); });
        check(text(read_bytes(dest)) == "preserve existing file");
        fs::remove(dest);
        std::uint64_t previous = 0;
        bool complete = false, publishing = false;
        store.export_revision(revision, dest,
                              {{}, [&](const ProgressEvent &event) {
                                 if (event.phase == ProgressPhase::exporting) {
                                   check(event.bytes >= previous && event.total_bytes == data.size() + 17);
                                   previous = event.bytes;
                                   complete |= event.complete;
                                 } else if (event.phase == ProgressPhase::publishing) {
                                   check(complete && !fs::exists(dest));
                                   publishing = true;
                                 }
                               }});
        check(complete && publishing && previous == data.size() + 17);
        check(text(read_bytes(dest / "nested/asset.bin")) == data);
        check(text(read_bytes(dest / "script")) == "#!/bin/sh\nexit 0\n" &&
              fs::file_size(dest / "empty") == 0);
#ifndef _WIN32
        check((fs::status(dest / "script").permissions() & fs::perms::owner_exec) != fs::perms::none);
#endif
        check(store.refs() == before && store.fsck() > 0);
        no_staging(parent);
        // The lower-level staging helper also cleans up after a writer/IO error.
        fails([&] {
          atomic_directory(parent / "failed", [&](const fs::path &stage) {
            atomic_write(stage / "partial", bytes("partial"), false);
            throw Error("injected write failure");
          });
        });
        check(!fs::exists(parent / "failed"));
        no_staging(parent);
      }
    std::cout << "Transactional export, cancellation, collisions and executable modes passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
