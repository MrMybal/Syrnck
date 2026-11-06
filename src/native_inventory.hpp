#pragma once
#include "internal.hpp"
namespace syrnck {
// Shared selected-file namespace and v1 revision metadata lower bound. Scope
// replacements must validate their union with retained entries, not only each
// freshly enumerated subtree.
class NativeInventoryPaths {
  std::set<std::string> folded_;
  std::size_t metadata_bytes_ = 2;
public:
  void add(std::string_view path) {
    validate_path(path);
    require(folded_.insert(portable_fold(path)).second, "case-insensitive path collision");
    const auto minimum = path.size() + std::string_view(R"({"manifest":"","executable":true})").size() +
                         64 + 3 + (folded_.size() > 1 ? 1 : 0);
    require(minimum <= max_object_size - metadata_bytes_, "native workspace metadata exceeds 16 MiB");
    metadata_bytes_ += minimum;
  }
  void finish(std::stop_token stop) const {
    for (const auto &path : folded_) {
      throw_if_cancelled(stop);
      for (auto n = path.find('/'); n != path.npos; n = path.find('/', n + 1))
        require(!folded_.contains(path.substr(0, n)), "file/directory path collision");
    }
    throw_if_cancelled(stop);
  }
};
} // namespace syrnck
