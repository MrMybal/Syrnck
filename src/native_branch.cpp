#include "native_branch.hpp"
namespace syrnck {
Json Store::native_branches(const std::optional<std::string> &after, std::size_t limit, bool verify_objects,
                            OperationOptions options) const {
  throw_if_cancelled(options.stop);
  require(limit >= 1 && limit <= 1000, "native branch page limit must be in 1..1000");
  if (after)
    native_branch_name(*after);
  const auto all = refs();
  const auto head = native_head();
  auto it = after ? all.upper_bound("heads/" + *after) : all.lower_bound("heads/");
  Json branches = Json::array();
  std::vector<ObjectKey> roots;
  std::size_t budget = 4096;
  for (; it != all.end() && it->first.starts_with("heads/") && branches.size() < limit; ++it) {
    throw_if_cancelled(options.stop);
    const NativeBranchState branch(*this, all, it->first.substr(6), options);
    auto row = branch.json(head["branch"] == branch.name);
    const auto size = row.dump().size() + 1;
    require(size <= max_object_size - budget, "native branch report exceeds 16 MiB");
    budget += size;
    branches.push_back(std::move(row));
    roots.push_back(branch.target);
    if (branch.index)
      roots.push_back(*branch.index);
    report_progress(options, {ProgressPhase::planning, branch.name, branches.size()});
  }
  if (verify_objects)
    (void)closure(roots, true, options.stop, options.progress);
  const bool more = it != all.end() && it->first.starts_with("heads/");
  const auto next = more && !branches.empty() ? branches.back()["name"] : Json{};
  report_progress(options, {ProgressPhase::planning, {}, branches.size(), branches.size(), true});
  return Json{{"version", 1},       {"branches", std::move(branches)},   {"head", head}, {"has_more", more},
              {"next_after", next}, {"objects_verified", verify_objects}};
}
Json Store::create_native_branch(std::string_view name, std::string_view revision,
                                 OperationOptions options) const {
  throw_if_cancelled(options.stop);
  native_branch_name(name);
  validate_id(revision);
  require_no_native_checkout(*this);
  const auto all = refs();
  const std::string ref = "heads/" + std::string(name), index = "staging/" + std::string(name);
  require(!all.contains(ref), "branch already exists");
  require(!all.contains(index), "native branch has an orphan staging index");
  const auto head = native_head();
  const auto value = metadata(Kind::revision, revision);
  const auto selection = native_branch_lookup(all, "workspace/HEAD");
  require(compare_update_refs({{ref, {}, ObjectKey{Kind::revision, std::string(revision)}},
                               {index, {}, {}},
                               {"workspace/HEAD", selection, selection}},
                              options),
          "native branch references changed; retry operation");
  return Json{{"version", 1},
              {"state", "created"},
              {"name", name},
              {"revision", revision},
              {"current", head["branch"].get<std::string>() == name},
              {"files", value["files"].size()}};
}
} // namespace syrnck
