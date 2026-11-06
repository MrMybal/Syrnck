#include "internal.hpp"
#include <algorithm>
namespace syrnck {
namespace {
constexpr std::array<std::string_view, 10> kinds{"reset",   "reset-index", "rebase-index", "stage-index",
                                                 "squash",  "amend",       "checkout",     "fetch",
                                                 "publish", "bisect"};
bool valid_kind(std::string_view kind) {
  return kind == "detached" || std::find(kinds.begin(), kinds.end(), kind) != kinds.end();
}
std::optional<std::string_view> classify(std::string_view name) {
  if (!name.starts_with("archive/"))
    return {};
  name.remove_prefix(8);
  if (name.size() == 64 && name.find('/') == name.npos)
    return "detached";
  for (const auto kind : kinds)
    if (name.starts_with(kind) && name.size() > kind.size() && name[kind.size()] == '/')
      return kind;
  return {};
}
std::string_view checked_kind(std::string_view name, const ObjectKey *key = nullptr) {
  validate_ref(name);
  const auto kind = classify(name);
  require(kind.has_value(), "unknown native history archive");
  require(name.size() == 72 + (*kind == "detached" ? 0 : kind->size() + 1),
          "invalid native history archive reference");
  const auto suffix = name.substr(name.rfind('/') + 1);
  validate_id(suffix);
  if (key) {
    require(key->kind == Kind::revision, "native history archive must point to a revision");
    require(*kind == "checkout" || key->id == suffix, "native history archive identity mismatch");
  }
  return *kind;
}
Json summary(const Store &store, std::string_view name, const ObjectKey &key) {
  const auto kind = checked_kind(name, &key);
  const auto value = store.metadata(Kind::revision, key.id);
  const auto message = value["message"].get<std::string>();
  auto end = std::min<std::size_t>(256, message.size());
  if (end < message.size())
    while (end && (static_cast<unsigned char>(message[end]) & 0xc0) == 0x80)
      --end;
  return Json{{"reference", name},
              {"kind", kind},
              {"revision", key.id},
              {"files", value["files"].size()},
              {"parents", value["parents"].size()},
              {"timestamp", value["timestamp"]},
              {"message", message.substr(0, end)},
              {"message_bytes", message.size()},
              {"message_truncated", end != message.size()}};
}
} // namespace
Json Store::native_archives(const std::optional<std::string> &kind, const std::optional<std::string> &after,
                            std::size_t limit, bool verify_objects, OperationOptions options) const {
  throw_if_cancelled(options.stop);
  require(limit >= 1 && limit <= 1000, "native archive page limit must be in 1..1000");
  require(!kind || valid_kind(*kind), "invalid native archive kind");
  if (after) {
    const auto category = checked_kind(*after);
    require(!kind || *kind == category, "native archive cursor does not match filter");
  }
  const auto all = refs();
  auto it = after ? all.upper_bound(*after) : all.lower_bound("archive/");
  Json rows = Json::array();
  std::vector<ObjectKey> roots;
  std::size_t budget = 4096;
  bool more = false;
  for (; it != all.end() && it->first.starts_with("archive/"); ++it) {
    throw_if_cancelled(options.stop);
    const auto category = classify(it->first);
    if (!category || (kind && *kind != *category))
      continue;
    (void)checked_kind(it->first, &it->second);
    if (rows.size() == limit) {
      more = true;
      break;
    }
    auto row = summary(*this, it->first, it->second);
    const auto size = row.dump().size() + 1;
    require(size <= max_object_size - budget, "native archive report exceeds 16 MiB");
    budget += size;
    rows.push_back(std::move(row));
    roots.push_back(it->second);
    report_progress(options, {ProgressPhase::planning, it->first, rows.size()});
  }
  if (verify_objects)
    (void)closure(roots, true, options.stop, options.progress);
  const auto next = more && !rows.empty() ? rows.back()["reference"] : Json{};
  report_progress(options, {ProgressPhase::planning, {}, rows.size(), rows.size(), true});
  return Json{{"version", 1},     {"archives", std::move(rows)}, {"kind", kind ? Json(*kind) : Json{}},
              {"has_more", more}, {"next_after", next},          {"objects_verified", verify_objects}};
}
Json Store::native_archive(std::string_view reference, bool verify_objects, OperationOptions options) const {
  throw_if_cancelled(options.stop);
  (void)checked_kind(reference);
  const auto all = refs();
  const auto found = all.find(std::string(reference));
  require(found != all.end(), "unknown native history archive");
  auto row = summary(*this, reference, found->second);
  if (verify_objects)
    (void)closure({found->second}, true, options.stop, options.progress);
  report_progress(options, {ProgressPhase::planning, {}, 1, 1, true});
  return Json{{"version", 1}, {"archive", std::move(row)}, {"objects_verified", verify_objects}};
}
} // namespace syrnck
