#include "internal.hpp"
#include <syrnck/git.hpp>
#include <charconv>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>

using namespace syrnck;
namespace {
void check(bool value) {
  require(value, "Git process contract test failed");
}
std::string packet(std::string_view value) {
  std::ostringstream out;
  out << std::hex << std::setfill('0') << std::setw(4) << value.size() + 4 << value;
  return out.str();
}
std::string handshake() {
  return packet("git-filter-client\n") + packet("version=2\n") + "0000" + packet("capability=clean\n") +
         packet("capability=smudge\n") + "0000";
}
std::string request(std::string_view content) {
  return packet("command=smudge\n") + packet("pathname=asset\n") + "0000" + packet(content) + "0000";
}
struct Replies {
  std::string_view value;
  std::optional<std::string_view> next() {
    check(value.size() >= 4);
    std::size_t size = 0;
    auto result = std::from_chars(value.data(), value.data() + 4, size, 16);
    check(result.ec == std::errc{} && result.ptr == value.data() + 4);
    value.remove_prefix(4);
    if (!size)
      return {};
    check(size >= 4 && size <= 65520 && value.size() >= size - 4);
    auto data = value.substr(0, size - 4);
    value.remove_prefix(size - 4);
    return data;
  }
  void greeting() {
    check(next() == "git-filter-server\n" && next() == "version=2\n" && !next());
    check(next() == "capability=clean\n" && next() == "capability=smudge\n" && !next());
  }
  void response(std::string_view expected, bool success = true) {
    check(next() == (success ? "status=success\n" : "status=error\n") && !next());
    if (!success)
      return;
    std::string decoded;
    while (auto data = next())
      decoded += *data;
    check(decoded == expected && !next());
  }
};
class ObservedOutput : public std::stringbuf {
public:
  bool active = false, early = false;
  std::istringstream &input;
  std::streamoff end;
  ObservedOutput(std::istringstream &in, std::streamoff size) : input(in), end(size) {}
  std::streamsize xsputn(const char *data, std::streamsize size) override {
    if (active && input.tellg() < end)
      early = true;
    return std::stringbuf::xsputn(data, size);
  }
};
} // namespace

int main() {
  auto root = fs::temp_directory_path() / path_from_utf8("syrnck-git-process-" + hex(random_bytes(16)));
  fs::create_directory(root);
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code ec;
      fs::remove_all(path, ec);
    }
  } cleanup{root};
  try {
    const std::string data(2 * 1024 * 1024 + 17, 'c');
    for (bool encrypted : {false, true}) {
      std::optional<Key> key;
      if (encrypted) {
        key.emplace();
        key->fill(0x37);
      }
      Store store(root / (encrypted ? "encrypted" : "plain"), key, true);
      std::istringstream file(data);
      auto [id, stats] = store.ingest(file, Chunking::fixed_size(512 * 1024));
      auto pointer = git::Pointer{id, stats.file_bytes}.encode();
      auto manifest = store.metadata(Kind::manifest, id);
      auto chunk = manifest["blocks"][0]["id"].get<std::string>();
      auto object = store.root() / "objects/chunk" / chunk.substr(0, 2) / chunk;
      auto good = read_bytes(object);
      auto bad = good;
      bad.back() ^= 1;
      store.clear_cache();
      store.reset_io_stats();
      auto script = handshake() + request(pointer) + request(pointer) + request(pointer) + request(pointer);
      std::istringstream input(script);
      std::ostringstream output;
      std::size_t calls = 0, errors = 0;
      std::vector<StoreIoStats> reads;
      git::filter_process(
          input, output,
          [&](std::string_view, std::string_view path, std::istream &source, std::ostream &destination) {
            check(path == "asset");
            ++calls;
            if (calls == 3)
              atomic_write(object, bad, true);
            if (calls == 4)
              atomic_write(object, good, true);
            git::smudge(store, source, destination);
            reads.push_back(store.io_stats());
          },
          {}, [&](std::string_view) { ++errors; });
      check(calls == 4 && errors == 1 && reads.size() == 3);
      check(reads[0].decoded_objects > 0 && reads[1].decoded_objects == reads[0].decoded_objects);
      check(reads[1].object_reads > reads[0].object_reads &&
            reads[1].cache_data_hits > reads[0].cache_data_hits);
      auto encoded = output.str();
      Replies replies{encoded};
      replies.greeting();
      replies.response(data);
      replies.response(data);
      replies.response({}, false);
      replies.response(data);
      check(replies.value.empty() && store.fsck() > 0);
    }
    // A handler which writes before consuming stdin must not cause an early
    // response. Large pass-through output takes the bounded spill path.
    const std::string raw(2 * 1024 * 1024, 'r');
    auto script = handshake() + request("body not yet consumed");
    std::istringstream input(script);
    ObservedOutput observed(input, static_cast<std::streamoff>(script.size()));
    std::ostream output(&observed);
    git::filter_process(
        input, output,
        [&](std::string_view, std::string_view, std::istream &source, std::ostream &destination) {
          observed.active = true;
          destination << raw;
          source.ignore(std::numeric_limits<std::streamsize>::max());
        });
    check(!observed.early);
    auto encoded = observed.str();
    Replies replies{encoded};
    replies.greeting();
    replies.response(raw);
    check(replies.value.empty());

    // A failure after content started must override success in the final list.
    std::istringstream late_input(script);
    std::ostringstream late_output;
    git::filter_process(
        late_input, late_output,
        [&](std::string_view, std::string_view, std::istream &source, std::ostream &destination) {
          source.ignore(std::numeric_limits<std::streamsize>::max());
          destination << "partial content";
          throw Error("late content failure");
        });
    check(
        late_output.str().ends_with(packet("partial content") + "0000" + packet("status=error\n") + "0000"));
    std::stop_source stop;
    std::istringstream stopped_input(script);
    std::ostringstream stopped_output;
    bool cancelled = false;
    try {
      git::filter_process(
          stopped_input, stopped_output,
          [&](std::string_view, std::string_view, std::istream &, std::ostream &) { stop.request_stop(); },
          {stop.get_token(), {}});
    } catch (const Cancelled &) {
      cancelled = true;
    }
    check(cancelled);
    std::cout << "Git process cache reuse, tamper checks, input gating and failure contracts passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
