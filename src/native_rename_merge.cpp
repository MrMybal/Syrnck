#include "native_rename_merge.hpp"
namespace syrnck {
namespace {
Json summary(const NativeSnapshot &files, const std::string &path) {
  const auto found = files.find(path);
  return found == files.end() ? Json{} : found->second.json();
}
void relocate(NativeSnapshot &files, const std::string &old, const std::string &next) {
  const auto found = files.find(old);
  if (found == files.end())
    return;
  require(!files.contains(next), "native rename alignment would replace another file");
  auto file = found->second;
  files.erase(found);
  files.emplace(next, std::move(file));
}
} // namespace
NativeRenameAlignment align_native_renames(const NativeSnapshot &base, const NativeSnapshot &current,
                                           const NativeSnapshot &incoming, NativeRenameBatch &batch,
                                           OperationOptions options) {
  const auto ours = match_native_renames(base, current, &batch, options);
  const auto theirs = match_native_renames(base, incoming, &batch, options);
  NativeRenameAlignment result;
  Json rows = Json::array();
  std::set<std::string> origins;
  for (const auto &[old, match] : ours.matches) {
    (void)match;
    origins.insert(old);
  }
  for (const auto &[old, match] : theirs.matches) {
    (void)match;
    origins.insert(old);
  }
  if (origins.empty()) {
    result.report = Json{{"current", ours.report}, {"incoming", theirs.report}, {"renames", Json::array()}};
    return result;
  }
  result.base = base;
  result.current = current;
  result.incoming = incoming;
  result.applied = true;
  std::size_t report_bytes = 4096;
  auto append = [&](Json &array, Json row) {
    const auto cost = row.dump().size() + 1;
    require(cost <= max_object_size - report_bytes, "native rename alignment report exceeds 16 MiB");
    report_bytes += cost;
    array.push_back(std::move(row));
  };
  for (const auto &old : origins) {
    throw_if_cancelled(options.stop);
    const auto a = ours.matches.find(old), b = theirs.matches.find(old);
    const bool our_rename = a != ours.matches.end(), their_rename = b != theirs.matches.end();
    const auto &next = our_rename ? a->second.target : b->second.target;
    const auto our_path = our_rename ? a->second.target : old;
    const auto their_path = their_rename ? b->second.target : old;
    std::string reason;
    if (our_rename && their_rename && our_path != their_path)
      reason = "rename_rename";
    else if (!our_rename && !current.contains(old))
      reason = "delete_rename";
    else if (!their_rename && !incoming.contains(old))
      reason = "rename_delete";
    else if ((!our_rename && current.contains(next)) || (!their_rename && incoming.contains(next)))
      reason = "rename_add";
    Json row{{"old_path", old},
             {"path", next},
             {"current_path", our_path},
             {"incoming_path", their_path},
             {"status", reason.empty() ? "aligned" : "conflict"},
             {"current_similarity", our_rename ? Json(a->second.similarity) : Json{}},
             {"incoming_similarity", their_rename ? Json(b->second.similarity) : Json{}}};
    if (!reason.empty()) {
      row["reason"] = reason;
      std::set<std::string> affected{old, next};
      if (reason == "rename_rename")
        affected.insert(their_path);
      for (const auto &path : affected)
        append(result.conflicts, Json{{"path", path},
                                      {"reason", reason},
                                      {"origin_path", old},
                                      {"base", summary(base, path)},
                                      {"current", summary(current, path)},
                                      {"incoming", summary(incoming, path)}});
    } else {
      relocate(result.base, old, next);
      if (!our_rename)
        relocate(result.current, old, next);
      if (!their_rename)
        relocate(result.incoming, old, next);
    }
    append(rows, std::move(row));
  }
  result.report = Json{{"current", ours.report}, {"incoming", theirs.report}, {"renames", std::move(rows)}};
  return result;
}
} // namespace syrnck
