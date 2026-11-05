#pragma once
#include "internal.hpp"
namespace syrnck::text_detail {
struct Limited {
  const char *reason;
};
struct Work {
  std::size_t remaining;
  const OperationOptions &options;
  void use(std::size_t n = 1) {
    throw_if_cancelled(options.stop);
    if (n > remaining)
      throw Limited{"work_limit"};
    remaining -= n;
  }
  bool equal(std::string_view a, std::string_view b) {
    use(a.size() == b.size() ? a.size() + 1 : 1);
    return a == b;
  }
};
using Lines = std::vector<std::string_view>;
struct Edit {
  char kind;
  std::string_view line;
};
Lines split(std::span<const std::uint8_t>, std::size_t limit, OperationOptions);
std::vector<Edit> edits(const Lines &, const Lines &, Work &);
} // namespace syrnck::text_detail
