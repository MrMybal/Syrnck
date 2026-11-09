#include <syrnck/net.hpp>
#include <syrnck/git.hpp>
#include <iostream>

int main() {
  try {
    syrnck::net::Remote remote("http://127.0.0.1:1", std::string(32, 't'));
    if (remote.endpoint_fingerprint().empty()) return 1;
    const auto encoded = syrnck::git::Pointer{std::string(64, '1'), 123}.encode();
    return syrnck::git::parse_pointer(encoded) ? 0 : 2;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 3;
  }
}
