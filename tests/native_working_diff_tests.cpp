#include "internal.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
using namespace syrnck;
namespace {
template <class F> void fails(F fn, std::string_view expected = {}) {
  try {
    fn();
  } catch (const Error &e) {
    require(expected.empty() || std::string_view(e.what()) == expected, "unexpected working diff error");
    return;
  }
  throw Error("expected working diff failure");
}
void write(const fs::path &path, std::string_view content) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
  require(out.good(), "working diff fixture write failed");
}
std::map<std::string, std::string> physical(const fs::path &root) {
  std::map<std::string, std::string> result;
  for (const auto &entry : fs::recursive_directory_iterator(root))
    if (entry.is_regular_file() && entry.path().filename() != "store.lock")
      result.emplace(utf8(entry.path().lexically_relative(root)), hash(read_bytes(entry.path())));
  return result;
}
} // namespace
int main() {
  const auto root = fs::temp_directory_path() / ("syrnck-working-diff-" + hex(random_bytes(16)));
  struct Cleanup {
    fs::path root;
    ~Cleanup() {
      std::error_code ec;
      fs::remove_all(root, ec);
    }
  } cleanup{root};
  try {
    for (bool encrypted : {false, true})
      for (unsigned layout = 0; layout < 3; ++layout) {
        const auto project = root / (std::to_string(encrypted) + '-' + std::to_string(layout));
        std::optional<Key> key;
        if (encrypted) {
          key.emplace();
          key->fill(39);
        }
        Store store(project / ".syrnck", key, true);
        const auto code = project / "Source/code.cpp";
        write(code, "old\n");
        write(project / "empty", "");
        write(project / ".syrnckignore", "Ignored/\n*.tmp\n!keep.tmp\n");
        const auto first = store.commit_directory(project, "main", "baseline");
        write(code, "prepared\n");
        const auto index = store.stage_directory(project, "main", {"Source/code.cpp"});
        if (layout)
          store.pack(layout == 1 ? PackOptions{} : PackOptions{512 * 1024 * 1024, 2});
        write(code, "working\r\n");
        write(project / "Unrelated/deep/huge.bin", std::string(2 * 1024 * 1024, 'x'));
        std::vector<std::string> scanned;
        const auto before = physical(store.root());
        const auto diff = store.diff_working_file(project, first, "Source/code.cpp", {},
                                                  {{}, [&](const ProgressEvent &event) {
                                                     if (event.phase == ProgressPhase::scanning)
                                                       scanned.emplace_back(event.path);
                                                   }});
        require(diff["comparison"] == "working" && diff["source_revision"] == first &&
                    diff["target_revision"].is_null() && diff["content_changed"] == true &&
                    diff["workspace_verified"] == true && diff["objects_verified"] == true &&
                    diff["target"]["file_hash"] == hash(bytes("working\r\n")),
                "working diff metadata differs");
        require(!scanned.empty(), "working diff omitted scanning progress");
        for (const auto &path : scanned)
          require(path == "Source" || path == "Source/code.cpp", "working diff traversed unrelated trees");
        const auto unstaged = store.diff_unstaged_file(project, "main", "Source/code.cpp");
        require(unstaged["source_revision"] == index && unstaged["index"] == index &&
                    unstaged["base"] == first && unstaged["stale"] == false &&
                    unstaged["diff"]["hunks"][0]["lines"][0]["text"] == "prepared",
                "working diff did not compare frozen staging bytes");
        require(physical(store.root()) == before, "working comparison wrote Store data");
        auto moved = store.metadata(Kind::revision, first);
        moved["parents"] = Json::array({first});
        moved["message"] = "head advances";
        const auto next = store.put(Kind::revision, bytes(moved.dump())).first;
        store.set_ref("heads/main", next);
        const auto saved = physical(store.root());
        const auto stale = store.diff_unstaged_file(project, "main", "Source/code.cpp");
        require(stale["stale"] == true && stale["head"] == next.id && stale["source_revision"] == index &&
                    stale["diff"] == unstaged["diff"],
                "stale working comparison substituted head");
        write(code, "prepared\n");
        const auto equal = store.diff_unstaged_file(project, "main", "Source/code.cpp");
        require(equal["equal"] == true && equal["diff"]["hunks"].empty(), "equal working diff differs");
#ifdef _WIN32
        require(equal["target"]["executable"].is_null() && equal["executable_comparable"] == false,
                "Windows working mode was treated as known");
#else
        fs::permissions(code, fs::perms::owner_exec, fs::perm_options::add);
        const auto mode = store.diff_unstaged_file(project, "main", "Source/code.cpp");
        require(mode["executable_changed"] == true && mode["content_changed"] == false &&
                    mode["equal"] == false,
                "working executable mode was lost");
        fs::permissions(code, fs::perms::owner_exec, fs::perm_options::remove);
#endif
        write(code, "old\n");
        TextDiffOptions small;
        small.max_file_bytes = 2;
        bool read_payload = false;
        const auto limited = store.diff_working_file(project, first, "Source/code.cpp", small,
                                                     {{}, [&](const ProgressEvent &event) {
                                                        read_payload |=
                                                            event.phase == ProgressPhase::reconstructing ||
                                                            event.phase == ProgressPhase::verifying;
                                                      }});
        require(!read_payload && limited["objects_verified"] == false &&
                    limited["workspace_verified"] == false && limited["equal"].is_null() &&
                    limited["content_changed"].is_null() && limited["target"]["file_hash"].is_null() &&
                    limited["diff"]["reason"] == "file_limit",
                "oversized working diff read payloads or invented equality");
        write(project / "new.cpp", "new\n");
        require(store.diff_unstaged_file(project, "main", "new.cpp")["source"].is_null(),
                "untracked file lost");
        const auto empty = store.diff_working_file(project, {}, "empty");
        require(empty["equal"] == false && empty["diff"]["equal"] == true,
                "empty untracked file lost presence");
        fs::remove(code);
        require(store.diff_unstaged_file(project, "main", "Source/code.cpp")["target"].is_null(),
                "working deletion lost");
        fails([&] { store.diff_working_file(project, {}, "missing.cpp"); },
              "native text diff path is absent from both snapshots");
        fails([&] { store.diff_working_file(project, first, "source/code.cpp"); },
              "native text diff path spelling differs from workspace");
        fails([&] { store.diff_working_file(project, first, "Source"); },
              "native text diff path is obstructed");
        fails([&] { store.diff_working_file(project, first, "../unsafe"); });
        fails([&] { store.diff_working_file(store.root(), first, "config.json"); },
              "native store must be below checkout workspace");
        fails([&] { store.diff_working_file(project, first, "Ignored/cache"); },
              "native text diff path is ignored");
        fails([&] { store.diff_working_file(project, first, "missing.tmp"); },
              "native text diff path is ignored");
        write(project / "keep.tmp", "kept");
        require(store.diff_working_file(project, {}, "keep.tmp")["diff"]["status"] == "text",
                "negative ignore rule failed");
        write(code, "old\n");
        write(project / "Source/.syrnckignore", "code.cpp\n");
        fails([&] { store.diff_working_file(project, first, "Source/code.cpp"); },
              "native text diff path is ignored");
        write(project / "Source/.syrnckignore", "code.cpp\n!code.cpp\n");
        require(store.diff_working_file(project, first, "Source/code.cpp")["equal"] == true,
                "nested negative ignore rule failed");
        write(project / "Source/.syrnckignore", std::string(4097, 'x') + '\n');
        fails([&] { store.diff_working_file(project, first, "Source/code.cpp"); },
              "native text diff ignore rule exceeds 4096 bytes");
        fs::remove(project / "Source/.syrnckignore");
        for (const auto &replacement :
             {std::string("short"), std::string("a longer working file than planned")}) {
          write(code, "original working bytes\n");
          bool changed = false;
          fails(
              [&] {
                store.diff_working_file(project, first, "Source/code.cpp", {},
                                        {{}, [&](const ProgressEvent &event) {
                                           if (!changed && event.phase == ProgressPhase::verifying) {
                                             changed = true;
                                             write(code, replacement);
                                           }
                                         }});
              },
              "file changed during native text diff; retry operation");
          require(changed, "mutation fixture did not run");
        }
        write(code, "same size");
        bool touched = false;
        fails(
            [&] {
              store.diff_working_file(project, first, "Source/code.cpp", {},
                                      {{}, [&](const ProgressEvent &event) {
                                         if (!touched && event.phase == ProgressPhase::verifying) {
                                           touched = true;
                                           fs::last_write_time(code, fs::last_write_time(code) +
                                                                         std::chrono::seconds(5));
                                         }
                                       }});
            },
            "file changed during native text diff; retry operation");
        std::stop_source stopped;
        fails(
            [&] {
              store.diff_working_file(project, first, "Source/code.cpp", {},
                                      {stopped.get_token(), [&](const ProgressEvent &event) {
                                         if (event.phase == ProgressPhase::verifying)
                                           stopped.request_stop();
                                       }});
            },
            "operation cancelled");
        fails(
            [&] {
              store.diff_working_file(project, first, "Source/code.cpp", {},
                                      {{}, [](const ProgressEvent &event) {
                                         if (event.phase == ProgressPhase::verifying)
                                           throw Error("working observer sentinel");
                                       }});
            },
            "working observer sentinel");
        require(physical(store.root()) == saved && store.refs().at("staging/main").id == index,
                "working inspection changed Store data or staging");
        const auto stable = physical(store.root());
        require(store.diff_unstaged_file(project, "main", "Source/code.cpp")["diff"]["status"] == "text" &&
                    physical(store.root()) == stable,
                "working diff wrote Store data");
        const auto custom_project = project / "custom";
        Store custom(custom_project / "data", key, true);
        fails([&] { custom.diff_working_file(custom_project, {}, "data/config.json"); },
              "native text diff cannot inspect protected storage");
      }
    std::cout
        << "Verified targeted working diffs, frozen/stale baselines, bounded reads and filesystem guards\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
