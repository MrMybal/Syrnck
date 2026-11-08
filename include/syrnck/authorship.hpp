#pragma once
#include <syrnck/signing.hpp>
#include <cstdint>
#include <string>
namespace syrnck {
struct NativeIdentity {
  std::string name, email;
  std::int32_t timezone_offset = 0; // minutes east of UTC
  void validate() const;
};
} // namespace syrnck
