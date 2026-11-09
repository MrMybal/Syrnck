#pragma once
#include "internal.hpp"
#include <unordered_map>
namespace syrnck {
struct NativeAncestryResult {
  std::vector<std::string> bases;
  bool source_is_ancestor = false, target_is_ancestor = false;
};
// Verified metadata is read once; repeated virtual-parent queries share the
// same child-before-parent topological order without writing synthetic nodes.
class NativeAncestryGraph {
public:
  NativeAncestryGraph(const Store &, const std::vector<std::string> &tips, NativeGraphLimits,
                      OperationOptions);
  NativeAncestryResult compare(const std::vector<std::string> &left, const std::vector<std::string> &right,
                               OperationOptions, const std::function<void(std::size_t)> &work = {}) const;
  Json report(std::string_view source, std::string_view target, OperationOptions) const;
  std::size_t size() const {
    return nodes_.size();
  }
  std::size_t parent_links() const {
    return links_;
  }

private:
  struct Node {
    std::string id;
    std::vector<std::size_t> parents;
  };
  std::vector<Node> nodes_;
  std::unordered_map<std::string, std::size_t> positions_;
  std::vector<std::size_t> order_;
  std::size_t links_ = 0;
};
} // namespace syrnck
