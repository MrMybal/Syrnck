#include <syrnck/core.hpp>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
using namespace syrnck;
namespace {
auto view(const std::string &s) {
  return std::span(reinterpret_cast<const std::uint8_t *>(s.data()), s.size());
}
template <class F> void fails(F function) {
  bool failed = false;
  try {
    function();
  } catch (const Error &) {
    failed = true;
  }
  require(failed, "expected range failure");
}
std::string slice(const Store &store, const std::string &id, std::uint64_t offset, std::uint64_t length,
                  OperationOptions options = {}) {
  std::ostringstream out;
  store.reconstruct_range(id, out, offset, length, options);
  return out.str();
}
fs::path chunk_path(const Store &store, const std::string &id) {
  return store.root() / "objects/chunk" / id.substr(0, 2) / id;
}
} // namespace
int main() {
  const auto root = fs::temp_directory_path() / ("syrnck-ranges-" + hex(random_bytes(16)));
  struct Cleanup {
    fs::path root;
    ~Cleanup() {
      std::error_code error;
      fs::remove_all(root, error);
    }
  } cleanup{root};
  try {
    std::string content;
    for (char c : {'a', 'b', 'c', 'd'})
      content += std::string(4096, c);
    for (bool encrypted : {false, true}) {
      std::optional<Key> key;
      if (encrypted) {
        key.emplace();
        key->fill(67);
      }
      for (int layout = 0; layout != 3; ++layout) {
        Store store(root / (std::to_string(encrypted) + "-" + std::to_string(layout)), key, true);
        std::istringstream input(content);
        const auto id = store.ingest(input, Chunking::fixed_size(4096)).first;
        store.set_ref("file", {Kind::manifest, id});
        if (layout) {
          PackOptions options;
          if (layout == 2)
            options.max_objects_per_pack = 1;
          store.pack(options);
        }
        store.clear_cache();
        store.reset_io_stats();
        require(slice(store, id, 4099, 91) == content.substr(4099, 91), "single chunk range differs");
        require(store.io_stats().object_reads == 2, "range read unrelated blocks");
        store.reset_io_stats();
        require(slice(store, id, 4089, 29) == content.substr(4089, 29), "cross chunk range differs");
        require(store.io_stats().object_reads == 3, "cross range read count differs");
        store.reset_io_stats();
        require(slice(store, id, content.size(), 0).empty(), "EOF range differs");
        require(store.io_stats().object_reads == 1, "empty range read chunks");
        require(slice(store, id, 0, content.size()) == content, "whole range differs");
        for (auto [offset, length] : {std::pair<std::uint64_t, std::uint64_t>{content.size() + 1, 0},
                                      {content.size(), 1},
                                      {1, UINT64_MAX},
                                      {UINT64_MAX, 2}})
          fails([&] { (void)slice(store, id, offset, length); });
        std::istringstream empty;
        const auto empty_id = store.ingest(empty).first;
        require(slice(store, empty_id, 0, 0).empty(), "empty file range differs");
        auto bad = store.metadata(Kind::manifest, id);
        bad["file_hash"] = std::string(64, '0');
        auto bad_id = store.put(Kind::manifest, view(bad.dump())).first.id;
        require(slice(store, bad_id, 1, 2) == content.substr(1, 2), "partial content differs");
        fails([&] { store.restore_file_range(bad_id, root / "bad-hash", 0, content.size()); });
        require(!fs::exists(root / "bad-hash"), "failed full range published output");
        auto output = root / ("slice-" + std::to_string(encrypted) + "-" + std::to_string(layout));
        store.restore_file_range(id, output, 4000, 300);
        std::ifstream restored(output, std::ios::binary);
        require(std::string(std::istreambuf_iterator<char>(restored), {}) == content.substr(4000, 300),
                "restored slice differs");
        fails([&] { store.restore_file_range(id, output, 0, 1); });
        std::stop_source stop;
        fails([&] {
          store.restore_file_range(id, root / "cancelled", 1, 9000,
                                   {stop.get_token(), [&](const ProgressEvent &event) {
                                      require(event.total_bytes == 9000, "range progress total differs");
                                      if (event.bytes)
                                        stop.request_stop();
                                    }});
        });
        require(!fs::exists(root / "cancelled"), "cancelled range published output");
        if (layout == 0) {
          auto blocks = store.metadata(Kind::manifest, id)["blocks"];
          auto unrelated = chunk_path(store, blocks[3]["id"].get<std::string>());
          fs::remove(unrelated);
          require(slice(store, id, 4099, 91) == content.substr(4099, 91), "unrelated block required");
          auto selected = chunk_path(store, blocks[1]["id"].get<std::string>());
          std::fstream corrupt(selected, std::ios::binary | std::ios::in | std::ios::out);
          char byte;
          corrupt.read(&byte, 1);
          byte ^= 1;
          corrupt.seekp(0);
          corrupt.write(&byte, 1);
          corrupt.close();
          fails([&] { store.restore_file_range(id, root / "corrupt", 4099, 91); });
          require(!fs::exists(root / "corrupt"), "corrupt range published output");
        }
      }
    }
    std::cout << "Verified ranges: loose/single/multiple packs, encryption, selective reads, integrity and "
                 "cancellation\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
