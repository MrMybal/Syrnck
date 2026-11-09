#include "native_graph.hpp"
#include <algorithm>
#include <deque>
namespace syrnck {
NativeAncestryGraph::NativeAncestryGraph(const Store &store, const std::vector<std::string> &tips,
                                         NativeGraphLimits limits, OperationOptions options,
                                         const std::function<void(const Json &)> &observer)
    : limits_(limits) {
  throw_if_cancelled(options.stop);
  require(!tips.empty(), "native graph requires at least one tip");
  require(limits.max_revisions >= 1 && limits.max_revisions <= 1000000,
          "native graph revision limit must be in 1..1000000");
  require(limits.max_parent_links <= 10000000, "native graph parent link limit must be in 0..10000000");
  auto insert = [&](const std::string &id) {
    const auto found = positions_.find(id);
    if (found != positions_.end())
      return found->second;
    require(nodes_.size() < limits.max_revisions, "native graph revision limit exceeded");
    const auto position = nodes_.size();
    nodes_.push_back(Node{id, {}});
    positions_.emplace(id, position);
    return position;
  };
  for (const auto &id : tips) {
    validate_id(id);
    insert(id);
  }
  report_progress(options, {ProgressPhase::planning, tips.front()});
  for (std::size_t i = 0; i < nodes_.size(); ++i) {
    throw_if_cancelled(options.stop);
    const auto revision = store.metadata(Kind::revision, nodes_[i].id);
    if (observer)
      observer(revision);
    const auto &parents = revision["parents"];
    require(parents.size() <= limits.max_parent_links - links_, "native graph parent link limit exceeded");
    links_ += parents.size();
    std::vector<std::size_t> edges;
    edges.reserve(parents.size());
    for (const auto &parent : parents) {
      throw_if_cancelled(options.stop);
      edges.push_back(insert(parent.get<std::string>()));
    }
    nodes_[i].parents = std::move(edges);
    if ((i + 1) % 256 == 0)
      report_progress(options, {ProgressPhase::planning, nodes_[i].id, i + 1});
  }
  std::vector<std::size_t> children(nodes_.size());
  for (const auto &node : nodes_)
    for (auto parent : node.parents)
      ++children[parent];
  std::deque<std::size_t> ready;
  for (std::size_t i = 0; i < nodes_.size(); ++i)
    if (!children[i])
      ready.push_back(i);
  order_.reserve(nodes_.size());
  while (!ready.empty()) {
    throw_if_cancelled(options.stop);
    const auto position = ready.front();
    ready.pop_front();
    order_.push_back(position);
    for (const auto parent : nodes_[position].parents)
      if (--children[parent] == 0)
        ready.push_back(parent);
  }
  require(order_.size() == nodes_.size(), "native revision graph contains a cycle");
}
NativeAncestryResult NativeAncestryGraph::compare(const std::vector<std::string> &left,
                                                  const std::vector<std::string> &right,
                                                  OperationOptions options,
                                                  const std::function<void(std::size_t)> &work) const {
  throw_if_cancelled(options.stop);
  std::vector<unsigned char> masks(nodes_.size()), dominated(nodes_.size());
  auto mark = [&](const std::vector<std::string> &tips, unsigned char mask) {
    for (const auto &id : tips) {
      const auto found = positions_.find(id);
      require(found != positions_.end(), "native graph query tip is outside the loaded graph");
      masks[found->second] |= mask;
    }
  };
  mark(left, 1);
  mark(right, 2);
  NativeAncestryResult result;
  std::size_t processed = 0;
  const auto visit = [&](const auto position) {
    throw_if_cancelled(options.stop);
    const auto &node = nodes_[position];
    if (work)
      work(1 + node.parents.size());
    const bool common = masks[position] == 3;
    if (common && !dominated[position]) {
      require(result.bases.size() < (max_object_size - 4096) / 80, "native graph result exceeds 16 MiB");
      result.bases.push_back(node.id);
    }
    for (const auto parent : node.parents) {
      throw_if_cancelled(options.stop);
      masks[parent] |= masks[position];
      dominated[parent] |= dominated[position] || common;
    }
    if (++processed % 256 == 0)
      report_progress(options, {ProgressPhase::planning, node.id, processed, nodes_.size()});
  };
  for (auto it = appended_.rbegin(); it != appended_.rend(); ++it)
    visit(*it);
  for (const auto position : order_)
    visit(position);
  result.source_is_ancestor = !left.empty() && std::all_of(left.begin(), left.end(), [&](const auto &id) {
    return (masks[positions_.at(id)] & 2) != 0;
  });
  result.target_is_ancestor = !right.empty() && std::all_of(right.begin(), right.end(), [&](const auto &id) {
    return (masks[positions_.at(id)] & 1) != 0;
  });
  std::sort(result.bases.begin(), result.bases.end());
  report_progress(options, {ProgressPhase::planning, {}, processed, nodes_.size(), true});
  return result;
}
std::vector<std::string> NativeAncestryGraph::parents(std::string_view id) const {
  const auto found = positions_.find(std::string(id));
  require(found != positions_.end(), "native graph query tip is outside the loaded graph");
  std::vector<std::string> result;
  result.reserve(nodes_[found->second].parents.size());
  for (const auto parent : nodes_[found->second].parents)
    result.push_back(nodes_[parent].id);
  return result;
}
void NativeAncestryGraph::append(std::string id, const std::vector<std::string> &parents,
                                 OperationOptions options) {
  throw_if_cancelled(options.stop);
  validate_id(id);
  std::vector<std::size_t> edges;
  edges.reserve(parents.size());
  for (const auto &parent : parents) {
    throw_if_cancelled(options.stop);
    const auto found = positions_.find(parent);
    require(found != positions_.end(), "native graph candidate parent is outside the loaded graph");
    edges.push_back(found->second);
  }
  if (const auto found = positions_.find(id); found != positions_.end()) {
    require(nodes_[found->second].parents == edges, "native graph candidate parent edges changed");
    return;
  }
  require(nodes_.size() < limits_.max_revisions, "native graph revision limit exceeded");
  require(edges.size() <= limits_.max_parent_links - links_, "native graph parent link limit exceeded");
  throw_if_cancelled(options.stop);
  const auto index = nodes_.size();
  nodes_.push_back(Node{std::move(id), std::move(edges)});
  positions_.emplace(nodes_.back().id, index);
  appended_.push_back(index);
  links_ += parents.size();
}
std::vector<std::string> NativeAncestryGraph::range(std::string_view base, std::string_view head,
                                                    std::size_t limit, OperationOptions options) const {
  const auto left = positions_.find(std::string(base)), right = positions_.find(std::string(head));
  require(left != positions_.end() && right != positions_.end(),
          "native graph query tip is outside the loaded graph");
  std::vector<unsigned char> excluded(nodes_.size()), visited(nodes_.size());
  std::vector<std::size_t> pending{left->second};
  while (!pending.empty()) {
    throw_if_cancelled(options.stop);
    const auto index = pending.back();
    pending.pop_back();
    if (excluded[index])
      continue;
    excluded[index] = 1;
    for (const auto parent : nodes_[index].parents)
      pending.push_back(parent);
  }
  struct Frame {
    std::size_t node, remaining;
  };
  std::vector<Frame> stack;
  std::vector<std::string> result;
  auto enter = [&](std::size_t index) {
    if (excluded[index] || visited[index])
      return;
    require(stack.size() + result.size() < limit, "native rebase commit limit exceeded");
    visited[index] = 1;
    stack.push_back({index, nodes_[index].parents.size()});
  };
  enter(right->second);
  while (!stack.empty()) {
    throw_if_cancelled(options.stop);
    auto &frame = stack.back();
    if (frame.remaining) {
      // Side parents first leaves the first-parent branch next to its merge.
      const auto parent = nodes_[frame.node].parents[--frame.remaining];
      enter(parent);
    } else {
      result.push_back(nodes_[frame.node].id);
      stack.pop_back();
    }
  }
  return result;
}
Json NativeAncestryGraph::report(std::string_view source, std::string_view target,
                                 OperationOptions options) const {
  auto result = compare({std::string(source)}, {std::string(target)}, options);
  return Json{{"version", 1},
              {"source", source},
              {"target", target},
              {"source_is_ancestor", result.source_is_ancestor},
              {"target_is_ancestor", result.target_is_ancestor},
              {"related", !result.bases.empty()},
              {"merge_bases", std::move(result.bases)},
              {"visited_revisions", nodes_.size()},
              {"parent_links", links_}};
}
Json Store::native_ancestry(std::string_view source, std::string_view target, NativeGraphLimits limits,
                            OperationOptions options) const {
  throw_if_cancelled(options.stop);
  validate_id(source);
  validate_id(target);
  return NativeAncestryGraph(*this, {std::string(source), std::string(target)}, limits, options)
      .report(source, target, options);
}
} // namespace syrnck
