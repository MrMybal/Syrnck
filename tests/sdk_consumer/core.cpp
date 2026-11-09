#include <syrnck/core.hpp>
#include <syrnck/git.hpp>
#include <syrnck/keys.hpp>
#include <syrnck/signing_keys.hpp>
#include <algorithm>
#include <iostream>

int main(int argc, char **argv) {
  try {
    if (argc != 2) return 1;
    auto secret = syrnck::SecretKey::generate();
    syrnck::Key key{};
    std::copy(secret.view().begin(), secret.view().end(), key.begin());
    syrnck::Bytes bytes(256 * 1024, 77);
    const syrnck::Bytes password(32, 17);
    const auto protected_signer = syrnck::generate_protected_signing_key(password);
    const auto signer = syrnck::open_protected_signing_key(protected_signer.envelope, password);
    if (signer.public_key() != protected_signer.public_key ||
        !syrnck::verify_ed25519(protected_signer.public_key, signer.sign(bytes), bytes)) return 7;
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
