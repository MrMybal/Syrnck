#include <chrono>
#include <functional>
#include <iostream>
#include <sstream>
#include <syrnck/core.hpp>
using namespace syrnck;
static void check(bool value) {
  require(value, "test check failed");
}
static void fails(const std::function<void()> &f) {
  bool failed = false;
  try {
    f();
  } catch (const std::exception &) {
    failed = true;
  }
  check(failed);
}
static auto view(const std::string &s) {
  return std::span(reinterpret_cast<const std::uint8_t *>(s.data()), s.size());
}
int main() {
  auto root = fs::temp_directory_path() / path_from_utf8("syrnck-core-" + hex(random_bytes(16)));
  fs::create_directory(root);
  struct Cleanup {
    fs::path p;
    ~Cleanup() {
      std::error_code ec;
      fs::remove_all(p, ec);
    }
  } cleanup{root};
  std::size_t count = 0;
  auto test = [&](std::string_view name, const std::function<void()> &f) {
    try {
      f();
      ++count;
      std::cout << "PASS " << name << '\n';
    } catch (const std::exception &e) {
      std::cerr << "FAIL " << name << ": " << e.what() << '\n';
      throw;
    }
  };
  try {
    Store store(root / "store", {}, true);
    test("exclusive store lock", [&] { fails([&] { Store other(root / "store"); }); });
    test("invalid object IDs", [&] {
      for (auto s : {"../config.json", "", "ABC"})
        fails([&] { validate_id(s); });
    });
    test("portable paths", [&] {
      for (auto s : {"../asset", "/root", "C:/asset", "a\\b", ".git/config", ".GIT/config",
                     ".syrnck/refs.json", "CON.txt", "foo/NUL", "a//b", "file.", "a:b"})
        fails([&] { validate_path(s); });
      validate_path("Content/Éditeur.uasset");
      fails([&] {
        validate_path("a\xc2\x85"
                      "b");
      });
    });
    test("chunk size validation", [&] {
      for (auto size : {0U, 16777217U})
        fails([&] { (void)Chunking::fixed_size(size); });
      Chunking c;
      c.min = 65;
      fails([&] { c.validate(); });
    });
    std::istringstream input("complete file data");
    auto [id, stats] = store.ingest(input);
    (void)stats;
    ObjectKey root_key{Kind::manifest, id};
    test("compare-and-swap", [&] {
      check(store.compare_set_ref("main", {}, root_key));
      check(!store.compare_set_ref("main", {}, root_key));
      check(store.compare_set_ref("main", root_key, root_key));
    });
    test("missing roots", [&] {
      fails([&] { store.set_ref("missing", {Kind::manifest, std::string(64, '0')}); });
      check(!store.refs().contains("missing"));
    });
    test("canonical JSON", [&] {
      auto j = store.metadata(Kind::manifest, id);
      auto pretty = j.dump(2);
      fails([&] { store.put(Kind::manifest, view(pretty)); });
      j["unknown"] = true;
      auto bad = j.dump();
      fails([&] { store.put(Kind::manifest, view(bad)); });
    });
    test("mismatched hash import", [&] {
      std::string data = "different bytes";
      fails([&] { store.import(root_key, view(data)); });
    });
    test("inconsistent full file hash is never published", [&] {
      auto j = store.metadata(Kind::manifest, id);
      j["file_hash"] = std::string(64, '0');
      auto value = j.dump();
      auto bad = store.put(Kind::manifest, view(value)).first;
      fails([&] { store.set_ref("bad", bad); });
      fails([&] { store.restore_file(bad.id, root / "bad-file"); });
      check(!fs::exists(root / "bad-file"));
    });
    test("Unicode and directory collisions", [&] {
      for (auto names :
           {std::pair{"Foo", "foo"}, std::pair{"Étage", "étage"}, std::pair{"file", "file/child"}}) {
        Json files = Json::object();
        for (auto path : {names.first, names.second})
          files[path] = Json{{"manifest", id}, {"executable", false}};
        auto value = Json{
            {"version", 1},
            {"parents", Json::array()},
            {"message", "test"},
            {"timestamp", 1},
            {"files",
             files}}.dump();
        fails([&] { store.put(Kind::revision, view(value)); });
      }
    });
    test("input errors do not publish manifests", [&] {
      std::istringstream broken("test");
      broken.setstate(std::ios::badbit);
      fails([&] { store.ingest(broken); });
    });
    test("empty chunks rejected", [&] { fails([&] { store.put(Kind::chunk, {}); }); });
    std::cout << count << " native C++ checks passed\n";
    return 0;
  } catch (...) {
    return 1;
  }
}
