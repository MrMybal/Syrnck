#include "native_history_fixture.hpp"
#include <syrnck.h>
#include <iostream>
using namespace native_test;
namespace {
std::map<std::string, std::string> physical(const fs::path &root) {
  std::map<std::string, std::string> result;
  for (const auto &entry : fs::recursive_directory_iterator(root))
    if (entry.is_regular_file() && entry.path().filename() != "store.lock")
      result.emplace(utf8(entry.path().lexically_relative(root)), hash(read_bytes(entry.path())));
  return result;
}
void verify(const fs::path &root, std::optional<Key> key, unsigned layout) {
  std::string good, bad;
  {
    Fixture fixture(root, key);
    good = fixture.file(std::string(150000, 'x'))["manifest"].get<std::string>();
    fixture.store.set_ref("keep", {Kind::manifest, good});
    fixture.pack(layout);
    auto metadata = fixture.store.metadata(Kind::manifest, good);
    metadata["file_hash"] = std::string(64, '0');
    bad = fixture.store.put(Kind::manifest, bytes(metadata.dump())).first.id;
  }
  const auto before = physical(root);
  SyrnckStore *store = nullptr;
  const auto path = utf8(root / ".syrnck");
  require(
      syrnck_store_open(path.c_str(), false, key ? key->data() : nullptr, key ? key->size() : 0, &store) == 0,
      "file verify ABI could not open its fixture");
  struct Close {
    SyrnckStore *value;
    ~Close() {
      syrnck_store_close(value);
    }
  } close{store};
  require(syrnck_file_verify(store, good.c_str()) == 0, "file verify ABI rejected a complete file");
  // A partial range cannot detect the intentionally incorrect whole-file hash.
  std::array<unsigned char, 1> output{};
  std::size_t length = 0;
  require(syrnck_file_read_range(store, bad.c_str(), 0, 1, output.data(), output.size(), &length) == 0 &&
              output[0] == 'x',
          "file verify ABI fixture did not preserve valid chunk bytes");
  require(syrnck_file_verify(store, bad.c_str()) == 1 &&
              std::string(syrnck_last_error()).find("hash") != std::string::npos,
          "file verify ABI accepted an invalid whole-file hash");
  require(syrnck_file_verify(store, nullptr) == 1 && syrnck_file_verify(nullptr, good.c_str()) == 1,
          "file verify ABI accepted null inputs");
  require(physical(root) == before, "file verify ABI wrote files or references");
}
} // namespace
int main() {
  Temporary temporary;
  try {
    for (bool encrypted : {false, true})
      for (unsigned layout = 0; layout < 3; ++layout) {
        std::optional<Key> key;
        if (encrypted) {
          key.emplace();
          key->fill(73);
        }
        verify(temporary.path / (std::to_string(encrypted) + '-' + std::to_string(layout)), key, layout);
      }
    std::cout << "Targeted C ABI whole-file verification on loose/packed encrypted stores\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
