#pragma once
#include <syrnck/core.hpp>
namespace syrnck::git {
struct Pointer {
  std::string manifest;
  std::uint64_t size;
  std::string encode() const;
};
std::optional<Pointer> parse_pointer(std::string_view);
void clean(const Store &, std::istream &, std::ostream &);
void smudge(const Store &, std::istream &, std::ostream &);
fs::path common_store(const fs::path &directory);
void install(const fs::path &directory, const fs::path &executable, const fs::path &store);
} // namespace syrnck::git
