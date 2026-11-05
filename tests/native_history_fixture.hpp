#pragma once
#include "internal.hpp"
#include <source_location>
#include <sstream>
namespace native_test {
using namespace syrnck;
template <class F> void fails(F function, std::source_location location = std::source_location::current()) {
  bool failed = false;
  try {
    function();
  } catch (const Error &) {
    failed = true;
  }
  require(failed, "expected native history failure at line " + std::to_string(location.line()));
}
struct Fixture {
  fs::path project;
  Store store;
  unsigned sequence = 0;
  Fixture(fs::path path, std::optional<Key> key = {})
      : project(std::move(path)), store(project / ".syrnck", key, true) {}
  Json file(std::string_view text, bool executable = false, std::uint32_t chunk = 1024) const {
    std::istringstream input{std::string(text)};
    return Json{{"manifest", store.ingest(input, Chunking::fixed_size(chunk)).first},
                {"executable", executable}};
  }
  std::string revision(Json files, Json parents = Json::array()) {
    std::map<std::string, Json> sorted;
    for (const auto &[path, entry] : files.items())
      sorted.emplace(path, entry);
    Json ordered = Json::object();
    auto &entries = ordered.get_ref<Json::object_t &>();
    for (const auto &[path, entry] : sorted)
      entries.emplace_back(path, entry);
    return store
        .put(Kind::revision, bytes(Json{{"version", 1},
                                        {"parents", parents},
                                        {"message", "history fixture " + std::to_string(++sequence)},
                                        {"timestamp", 0},
                                        {"files", ordered}}
                                       .dump()))
        .first.id;
  }
  void write(const fs::path &path, std::string_view text) const {
    fs::create_directories((project / path).parent_path());
    atomic_write(project / path, bytes(text), true);
  }
  void install(const std::string &id) const {
    for (const auto &entry : fs::directory_iterator(project))
      if (entry.path().filename() != ".syrnck")
        fs::remove_all(entry.path());
    const auto value = store.metadata(Kind::revision, id);
    for (const auto &[path, entry] : value["files"].items()) {
      const auto destination = project / path_from_utf8(path);
      store.restore_file(entry["manifest"].get<std::string>(), destination);
      if (entry["executable"].get<bool>())
        make_executable(destination);
    }
    store.set_ref("heads/main", {Kind::revision, id});
  }
  std::map<std::string, std::string> physical() const {
    std::map<std::string, std::string> result;
    for (const auto &entry : fs::recursive_directory_iterator(project))
      if (entry.is_regular_file() && entry.path().filename() != "store.lock")
        result.emplace(utf8(entry.path().lexically_relative(project)), hash(read_bytes(entry.path())));
    return result;
  }
  void pack(unsigned physical) const {
    if (physical == 1)
      store.pack();
    if (physical == 2)
      store.pack({512 * 1024 * 1024, 3});
  }
};
struct Temporary {
  fs::path path = fs::temp_directory_path() / ("syrnck-history-" + hex(random_bytes(16)));
  ~Temporary() {
    std::error_code error;
    fs::remove_all(path, error);
  }
};
} // namespace native_test
