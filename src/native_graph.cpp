#include "internal.hpp"
#include <algorithm>
#include <deque>
#include <unordered_map>
namespace syrnck {
Json Store::native_ancestry(std::string_view source, std::string_view target, NativeGraphLimits limits,
                            OperationOptions options) const {
  throw_if_cancelled(options.stop);
  validate_id(source);
  validate_id(target);
  require(limits.max_revisions >= 1 && limits.max_revisions <= 1000000,
          "native graph revision limit must be in 1..1000000");
  require(limits.max_parent_links <= 10000000, "native graph parent link limit must be in 0..10000000");
  struct Node {
    std::string id;
    std::vector<std::size_t> parents;
    std::size_t children = 0;
    unsigned mask = 0;
    bool dominated = false;
  };
  std::vector<Node> nodes;
  std::unordered_map<std::string, std::size_t> positions;
  auto insert = [&](const std::string &id) {
    const auto found = positions.find(id);
    if (found != positions.end())
      return found->second;
    require(nodes.size() < limits.max_revisions, "native graph revision limit exceeded");
    const auto position = nodes.size();
    nodes.push_back(Node{id, {}});
    positions.emplace(id, position);
    return position;
  };
  const auto left = insert(std::string(source)), right = insert(std::string(target));
  nodes[left].mask |= 1;
  nodes[right].mask |= 2;
  std::size_t links = 0;
  report_progress(options, {ProgressPhase::planning, source});
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    throw_if_cancelled(options.stop);
    const auto revision = metadata(Kind::revision, nodes[i].id);
    const auto &parents = revision["parents"];
    require(parents.size() <= limits.max_parent_links - links, "native graph parent link limit exceeded");
    links += parents.size();
    std::vector<std::size_t> edges;
    edges.reserve(parents.size());
    for (const auto &parent : parents) {
      throw_if_cancelled(options.stop);
      const auto position = insert(parent.get<std::string>());
      ++nodes[position].children;
      edges.push_back(position);
    }
    nodes[i].parents = std::move(edges);
    if ((i + 1) % 256 == 0)
      report_progress(options, {ProgressPhase::planning, nodes[i].id, i + 1});
  }
  std::deque<std::size_t> ready;
  for (std::size_t i = 0; i < nodes.size(); ++i)
    if (nodes[i].children == 0)
      ready.push_back(i);
  std::vector<std::string> bases;
  std::size_t processed = 0;
  while (!ready.empty()) {
    throw_if_cancelled(options.stop);
    const auto position = ready.front();
    ready.pop_front();
    const auto &node = nodes[position];
    const bool common = node.mask == 3;
    if (common && !node.dominated) {
      require(bases.size() < (max_object_size - 4096) / 80, "native graph result exceeds 16 MiB");
      bases.push_back(node.id);
    }
    for (const auto parent : node.parents) {
      throw_if_cancelled(options.stop);
      auto &next = nodes[parent];
      next.mask |= node.mask;
      next.dominated = next.dominated || node.dominated || common;
      if (--next.children == 0)
        ready.push_back(parent);
    }
    if (++processed % 256 == 0)
      report_progress(options, {ProgressPhase::planning, node.id, processed, nodes.size()});
  }
  require(processed == nodes.size(), "native revision graph contains a cycle");
  std::sort(bases.begin(), bases.end());
  report_progress(options, {ProgressPhase::planning, {}, processed, nodes.size(), true});
  return Json{{"version", 1},
              {"source", source},
              {"target", target},
              {"source_is_ancestor", (nodes[left].mask & 2) != 0},
              {"target_is_ancestor", (nodes[right].mask & 1) != 0},
              {"related", !bases.empty()},
              {"merge_bases", bases},
              {"visited_revisions", nodes.size()},
              {"parent_links", links}};
}
} // namespace syrnck
