#include "native_virtual_snapshots.hpp"
namespace syrnck {
NativeVirtualSnapshots::NativeVirtualSnapshots(const Store &store, std::size_t limit,
                                               OperationOptions options,
                                               std::function<void(std::size_t)> work)
    : store_(store), limit_(limit), options_(std::move(options)), work_(std::move(work)) {
  require(limit >= 1024 && limit <= 256 * 1024 * 1024,
          "native rebase delta limit must be in 1024..268435456");
}
void NativeVirtualSnapshots::work(std::size_t amount) {
  throw_if_cancelled(options_.stop);
  if (work_)
    work_(amount);
}
NativeSnapshot NativeVirtualSnapshots::read(std::string_view id) {
  work(1);
  if (recent_id_ == id) {
    work(recent_.size());
    return recent_;
  }
  std::optional<std::string> root{std::string(id)};
  std::vector<const Delta *> chain;
  while (root) {
    work(1);
    const auto found = records_.find(*root);
    if (found == records_.end())
      break;
    chain.push_back(&found->second);
    root = found->second.parent;
  }
  auto result = root ? read_native_snapshot(store_, *root, options_) : NativeSnapshot{};
  work(result.size());
  for (auto it = chain.rbegin(); it != chain.rend(); ++it)
    for (const auto &[path, file] : (*it)->files) {
      work(1);
      if (file)
        result.insert_or_assign(path, *file);
      else
        result.erase(path);
    }
  return result;
}
void NativeVirtualSnapshots::add(std::string id, std::optional<std::string> parent,
                                 const NativeSnapshot &after) {
  work(1);
  validate_id(id);
  if (parent) {
    validate_id(*parent);
    require(id != *parent, "native virtual snapshot cannot parent itself");
  }
  require(!records_.contains(id), "duplicate native virtual snapshot");
  // A prior interrupted apply may already have materialized this exact ID.
  // Read that immutable object normally rather than shadowing it with a delta.
  if (store_.contains({Kind::revision, id}))
    return;
  const auto before = parent ? read(*parent) : NativeSnapshot{};
  // A virtual parent must already exist. Real parents are verified by the
  // caller's loaded ancestry graph; this class never publishes objects.
  Delta delta{std::move(parent), {}};
  std::size_t cost = 160;
  auto left = before.begin(), right = after.begin();
  while (left != before.end() || right != after.end()) {
    work(1);
    const bool have_left = left != before.end() && (right == after.end() || left->first <= right->first);
    const bool have_right = right != after.end() && (left == before.end() || right->first <= left->first);
    const auto &path = have_left ? left->first : right->first;
    const auto *a = have_left ? &left->second : nullptr, *b = have_right ? &right->second : nullptr;
    if (!a || !b || a->manifest != b->manifest || a->content != b->content ||
        a->executable != b->executable) {
      const auto bytes = Json(path).dump().size() + 2 + (b ? b->json().dump().size() : 4);
      require(cost <= limit_ - bytes_ && bytes <= limit_ - bytes_ - cost,
              "native rebase delta limit exceeded");
      cost += bytes;
      delta.files.emplace_hint(delta.files.end(), path, b ? std::optional{*b} : std::nullopt);
    }
    if (have_left)
      ++left;
    if (have_right)
      ++right;
  }
  require(cost <= limit_ - bytes_, "native rebase delta limit exceeded");
  work(after.size());
  auto recent = after;
  const auto count = delta.files.size();
  records_.emplace(id, std::move(delta));
  recent_id_ = std::move(id);
  recent_ = std::move(recent);
  bytes_ += cost;
  entries_ += count;
}
Json NativeVirtualSnapshots::report() const {
  return Json{{"virtual_snapshots", records_.size()},
              {"delta_entries", entries_},
              {"delta_bytes", bytes_},
              {"max_delta_bytes", limit_},
              {"cached_files", recent_.size()}};
}
} // namespace syrnck
