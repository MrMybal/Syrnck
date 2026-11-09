#include <syrnck/core.hpp>
#include <syrnck/git.hpp>
#include <syrnck/keys.hpp>
#include <algorithm>
#include <iostream>

int main(int argc, char **argv) {
  try {
    if (argc != 2) return 1;
    auto secret = syrnck::SecretKey::generate();
    syrnck::Key key{};
    std::copy(secret.view().begin(), secret.view().end(), key.begin());
    syrnck::Bytes bytes(256 * 1024, 77);
    syrnck::ObjectKey retained{};
    {
      syrnck::Store store(syrnck::path_from_utf8(argv[1]), key, true);
      retained = store.put(syrnck::Kind::chunk, bytes).first;
      if (!store.encrypted() || store.get(retained) != bytes) return 2;
      const auto pointer = syrnck::git::Pointer{retained.id, bytes.size()}.encode();
      auto parsed = syrnck::git::parse_pointer(pointer);
      if (!parsed || parsed->manifest != retained.id || parsed->size != bytes.size()) return 3;
      if (syrnck::parse_key(syrnck::Json::parse(syrnck::key_json(retained).dump())) != retained) return 4;
    }
    syrnck::Store reopened(syrnck::path_from_utf8(argv[1]), key);
    syrnck::wipe_secret(key);
    if (reopened.get(retained) != bytes) return 5;
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 6;
  }
}
