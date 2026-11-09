#include "sealed_internal.hpp"
#include <algorithm>

namespace syrnck::sealed_detail {
Index::Index(const Store &wire, Budget &budget) : wire_(wire), budget_(budget) {}
std::shared_ptr<const Node> Index::read(const std::string &id, const std::string &prefix) {
  budget_.work();
  if (const auto found = cache_.find(id); found != cache_.end()) {
    require(found->second.node->prefix == prefix, "sealed index prefix mismatch");
    recent_.splice(recent_.begin(), recent_, found->second.at);
    return found->second.node;
  }
  const auto value = wire_.metadata(Kind::revision, id);
  ++budget_.index_reads;
  const auto message = value["message"].get<std::string>();
  const auto marker = std::string(index_message) + prefix + ':';
  require(value["timestamp"] == 0 && message.starts_with(marker), "invalid sealed index node");
  const auto labels = message.substr(marker.size());
  require(prefix.size() <= 64 && labels.size() <= 16 && labels.size() == value["parents"].size(),
          "invalid sealed index children");
  auto node = std::make_shared<Node>();
  node->prefix = prefix;
  if (!labels.empty()) {
    require(prefix.size() < 64 && value["files"].empty(), "invalid sealed index branch");
    char before = 0;
    for (std::size_t i = 0; i < labels.size(); ++i) {
      const char label = labels[i];
      require(label > before && std::string_view("0123456789abcdef").find(label) != std::string_view::npos,
              "invalid sealed index child order");
      before = label;
      node->children.emplace(label, value["parents"][i].get<std::string>());
    }
  } else {
    require(value["files"].size() <= leaf_size, "sealed index leaf exceeds 256 entries");
    for (const auto &[path, file] : value["files"].items()) {
      budget_.work();
      require(path.starts_with(object_path), "invalid sealed index object path");
      const auto address = path.substr(object_path.size());
      validate_id(address);
      require(address.starts_with(prefix) && file["executable"] == false, "invalid sealed index entry");
      node->entries.emplace(address, file["manifest"].get<std::string>());
    }
  }
  const auto size = value.dump().size() + node->entries.size() * 96 + 256;
  if (size <= budget_.limits.max_index_cache_bytes) {
    while (size > budget_.limits.max_index_cache_bytes - bytes_) {
      const auto last = cache_.find(recent_.back());
      bytes_ -= last->second.bytes;
      cache_.erase(last);
      recent_.pop_back();
    }
    recent_.push_front(id);
    cache_.emplace(id, Cached{node, size, recent_.begin()});
    bytes_ += size;
  }
  return node;
}
std::optional<std::string> Index::find(const std::string &root, const std::string &address) {
  validate_id(address);
  std::string id = root, prefix;
  for (;;) {
    const auto node = read(id, prefix);
    if (node->children.empty()) {
      const auto at = node->entries.find(address);
      return at == node->entries.end() ? std::nullopt : std::optional{at->second};
    }
    const char label = address[prefix.size()];
    const auto child = node->children.find(label);
    if (child == node->children.end())
      return {};
    id = child->second;
    prefix.push_back(label);
  }
}
std::string Index::write(const Store &author, const Node &node) {
  budget_.work(1 + node.entries.size() + node.children.size());
  Json parents = Json::array(), files = Json::object();
  std::string labels;
  for (const auto &[label, id] : node.children) {
    labels += label;
    parents.push_back(id);
  }
  auto &entries = files.get_ref<Json::object_t &>();
  entries.reserve(node.entries.size());
  for (const auto &[address, manifest] : node.entries)
    entries.emplace_back(std::string(object_path) + address,
                         Json{{"manifest", manifest}, {"executable", false}});
  auto value =
      author.native_commit_metadata(Json{{"version", 1},
                                         {"parents", std::move(parents)},
                                         {"message", std::string(index_message) + node.prefix + ':' + labels},
                                         {"timestamp", 0},
                                         {"files", std::move(files)}});
  const auto result = wire_.put(Kind::revision, bytes(value.dump())).first;
  ++budget_.index_writes;
  return result.id;
}
std::string Index::rewrite(const Store &author, const std::optional<std::string> &old,
                           const std::string &prefix, const std::vector<Change> &changes, std::size_t first,
                           std::size_t last, const Entries *seed) {
  budget_.work();
  if (first == last && old)
    return *old;
  const auto existing = old ? read(*old, prefix) : std::shared_ptr<const Node>{};
  if (existing && existing->children.empty())
    seed = &existing->entries;
  const bool branch = existing && !existing->children.empty();
  const auto count = (seed ? seed->size() : 0) + last - first;
  if (!branch && count <= leaf_size) {
    Node node{prefix, seed ? *seed : Entries{}, {}};
    for (std::size_t i = first; i < last; ++i)
      node.entries.insert_or_assign(changes[i].address, changes[i].manifest);
    return write(author, node);
  }
  require(prefix.size() < 64, "sealed index depth exceeded");
  Node node{prefix, {}, {}};
  std::size_t at = first;
  for (const char label : std::string_view("0123456789abcdef")) {
    budget_.work();
    const auto begin = at;
    while (at < last && changes[at].address[prefix.size()] == label) {
      budget_.work();
      ++at;
    }
    std::optional<std::string> child;
    if (branch)
      if (const auto found = existing->children.find(label); found != existing->children.end())
        child = found->second;
    Entries inherited;
    if (seed)
      for (const auto &[address, manifest] : *seed) {
        budget_.work();
        if (address[prefix.size()] == label)
          inherited.emplace(address, manifest);
      }
    if (child || begin != at || !inherited.empty())
      node.children.emplace(label, rewrite(author, child, prefix + label, changes, begin, at,
                                           inherited.empty() ? nullptr : &inherited));
  }
  require(at == last, "invalid sealed index update order");
  return write(author, node);
}
std::string Index::update(const Store &author, std::optional<std::string> root,
                          const std::vector<Change> &changes) {
  for (std::size_t i = 0; i < changes.size(); ++i) {
    budget_.work();
    validate_id(changes[i].address);
    validate_id(changes[i].manifest);
    require(i == 0 || changes[i - 1].address < changes[i].address, "invalid sealed index update order");
  }
  return rewrite(author, root, "", changes, 0, changes.size());
}
} // namespace syrnck::sealed_detail
