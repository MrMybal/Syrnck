#include "server_access.hpp"
#include <algorithm>
#include <monocypher.h>
namespace syrnck::net::detail {
namespace {
void allow(bool value) {
  if (!value)
    throw AccessDenied("server permission denied");
}
void component(std::string_view value) {
  validate_ref(value);
  require(value.find('/') == value.npos && value.size() <= 128, "invalid server access component");
}
std::vector<std::string> strings(const Json &value, bool paths) {
  require(value.is_array() && value.size() <= 1024, "invalid server access list");
  std::set<std::string> unique;
  std::vector<std::string> result;
  for (const auto &entry : value) {
    require(entry.is_string(), "invalid server access list");
    auto item = entry.get<std::string>();
    require(item.size() <= 4096, "invalid server access list");
    if (paths) {
      if (!item.empty())
        validate_path(item);
      item = portable_fold(item);
    } else
      component(item);
    require(unique.insert(item).second, "duplicate server access entry");
    result.push_back(std::move(item));
  }
  return result;
}
bool within(std::string_view path, const std::vector<std::string> &prefixes) {
  const auto folded = portable_fold(path);
  return std::any_of(prefixes.begin(), prefixes.end(), [&](const auto &prefix) {
    return prefix.empty() || folded == prefix ||
           (folded.starts_with(prefix) && folded.size() > prefix.size() && folded[prefix.size()] == '/');
  });
}
constexpr std::size_t max_revisions = 100000, max_links = 1000000, max_paths = 16384;
std::set<std::string> ancestors(const Store &store, const std::optional<ObjectKey> &root) {
  std::set<std::string> result;
  std::vector<std::string> pending;
  if (root)
    pending.push_back(root->id);
  std::size_t links = 0;
  while (!pending.empty()) {
    auto id = std::move(pending.back());
    pending.pop_back();
    if (!result.insert(id).second)
      continue;
    require(result.size() <= max_revisions, "server publication history limit exceeded");
    const auto value = store.metadata(Kind::revision, id);
    const auto &parents = value["parents"];
    require(parents.size() <= max_links - links, "server publication history limit exceeded");
    links += parents.size();
    for (const auto &parent : parents)
      pending.push_back(parent.get<std::string>());
  }
  return result;
}
} // namespace
ServerAccess::ServerAccess(const Json &value) {
  require(value.dump().size() <= 1024 * 1024 && value.is_object() && value.size() == 3 &&
              value.contains("version") && value["version"].is_number_integer() && value["version"] == 1 &&
              value.contains("principals") && value["principals"].is_array() &&
              !value["principals"].empty() && value["principals"].size() <= 256 &&
              value.contains("branches") && value["branches"].is_array() && value["branches"].size() <= 1024,
          "invalid server access configuration");
  std::set<std::string> identities, digests;
  for (const auto &entry : value["principals"]) {
    require(entry.is_object() && entry.size() == 5 && entry.contains("id") && entry["id"].is_string() &&
                entry.contains("token_hash") && entry["token_hash"].is_string() && entry.contains("role") &&
                entry["role"].is_string() && entry.contains("write_paths") && entry.contains("branches"),
            "invalid server principal");
    Principal principal{entry["id"].get<std::string>(), entry["token_hash"].get<std::string>(),
                        entry["role"].get<std::string>(), strings(entry["write_paths"], true),
                        strings(entry["branches"], false)};
    component(principal.id);
    validate_id(principal.digest);
    require(identities.insert(portable_fold(principal.id)).second && digests.insert(principal.digest).second,
            "duplicate server principal");
    require(principal.role == "reader" || principal.role == "writer" || principal.role == "admin",
            "invalid server principal role");
    require(principal.role != "reader" || (principal.paths.empty() && principal.branches.empty()),
            "reader cannot have server write permissions");
    principals_.push_back(std::move(principal));
  }
  for (const auto &entry : value["branches"]) {
    require(entry.is_object() && entry.size() == 3 && entry.contains("name") && entry["name"].is_string() &&
                entry.contains("fast_forward") && entry["fast_forward"].is_boolean() &&
                entry.contains("lock_paths"),
            "invalid server branch policy");
    const auto name = entry["name"].get<std::string>();
    component(name);
    require(branches_
                .emplace(name,
                         BranchPolicy{entry["fast_forward"].get<bool>(), strings(entry["lock_paths"], true)})
                .second,
            "duplicate server branch policy");
  }
  for (const auto &principal : principals_)
    for (const auto &name : principal.branches)
      require(branches_.contains(name), "server principal names an unknown branch");
}
const Principal *ServerAccess::authenticate(std::string_view token) const {
  const auto digest = unhex(hash(bytes(token)));
  const Principal *found = nullptr;
  for (const auto &principal : principals_)
    if (crypto_verify32(digest.data(), unhex(principal.digest).data()) == 0)
      found = &principal;
  return found;
}
bool ServerAccess::visible(std::string_view name) const {
  return branches_.contains(std::string(name));
}
void ServerAccess::write_path(const Principal &principal, std::string_view path) const {
  allow(principal.role != "reader" && within(path, principal.paths));
}
void ServerAccess::write_branch(const Principal &principal, std::string_view name) const {
  allow(principal.role != "reader" && visible(name) &&
        std::find(principal.branches.begin(), principal.branches.end(), name) != principal.branches.end());
}
std::vector<AssetLockClaim> decode_lock_claims(const Json &value) {
  require(value.is_array() && value.size() <= 1024, "invalid server asset lock claims");
  std::vector<AssetLockClaim> result;
  std::set<std::string> paths;
  for (const auto &entry : value) {
    require(entry.is_object() && entry.size() == 2 && entry.contains("path") && entry["path"].is_string() &&
                entry.contains("fence") && entry["fence"].is_number_unsigned() && entry["fence"] != 0,
            "invalid server asset lock claims");
    auto path = entry["path"].get<std::string>();
    validate_path(path);
    require(path.size() <= 4096 && paths.insert(portable_fold(path)).second,
            "invalid server asset lock claims");
    result.push_back({std::move(path), entry["fence"].get<std::uint64_t>()});
  }
  return result;
}
std::function<void(const ProgressEvent &)>
ServerAccess::publication(const Store &store, const Principal &principal, std::string_view name,
                          const ObjectKey &root, const std::optional<ObjectKey> &previous,
                          const std::vector<AssetLockClaim> &claims) const {
  write_branch(principal, name);
  allow(root.kind == Kind::revision && (!previous || previous->kind == Kind::revision));
  const auto &policy = branches_.at(std::string(name));
  if (previous && policy.fast_forward)
    allow(store.native_ancestry(previous->id, root.id)["source_is_ancestor"].get<bool>());
  const auto known = ancestors(store, previous);
  std::set<std::string> seen;
  std::vector<std::string> pending{root.id};
  std::map<std::string, std::string> changed;
  std::size_t links = 0, path_bytes = 0;
  auto change = [&](const std::string &path) {
    write_path(principal, path);
    if (changed.emplace(portable_fold(path), path).second) {
      const auto cost = path.size() * 2 + 64;
      require(changed.size() <= max_paths && cost <= max_object_size - path_bytes,
              "server publication path limit exceeded");
      path_bytes += cost;
    }
  };
  while (!pending.empty()) {
    auto id = std::move(pending.back());
    pending.pop_back();
    if (known.contains(id) || !seen.insert(id).second)
      continue;
    require(seen.size() <= max_revisions, "server publication history limit exceeded");
    const auto revision = store.metadata(Kind::revision, id);
    const auto &files = revision["files"], &parents = revision["parents"];
    require(parents.size() <= max_links - links, "server publication history limit exceeded");
    links += parents.size();
    if (parents.empty())
      for (const auto &[path, file] : files.items()) {
        (void)file;
        change(path);
      }
    for (const auto &parent : parents) {
      const auto base_id = parent.get<std::string>();
      const auto base = store.metadata(Kind::revision, base_id);
      const auto &before = base["files"];
      for (const auto &[path, file] : files.items())
        if (!before.contains(path) || before[path] != file)
          change(path);
      for (const auto &[path, file] : before.items()) {
        (void)file;
        if (!files.contains(path))
          change(path);
      }
      pending.push_back(base_id);
    }
  }
  // A forced rewind still changes the branch's visible snapshot, even when
  // every target revision was already reachable from the old head.
  if (previous) {
    const auto comparison = store.compare_revisions(previous->id, root.id);
    for (const auto &row : comparison["changes"])
      change(row["path"].get<std::string>());
  }
  std::set<std::string> supplied;
  for (const auto &claim : claims)
    supplied.insert(portable_fold(claim.path));
  const auto now = unix_seconds();
  AssetLocks locks(store);
  std::set<std::string> active;
  std::optional<std::string> cursor;
  do {
    const auto page = locks.list(cursor, 1000);
    for (const auto &entry : page["locks"])
      if (entry["active"].get<bool>())
        active.insert(portable_fold(entry["path"].get<std::string>()));
    cursor = page["next"].is_null() ? std::nullopt : std::optional{page["next"].get<std::string>()};
  } while (cursor);
  for (const auto &[folded, path] : changed)
    allow((!within(path, policy.locks) && !active.contains(folded)) || supplied.contains(folded));
  std::uint64_t deadline = UINT64_MAX;
  if (!claims.empty()) {
    try {
      deadline = locks.validate(principal.id, claims);
    } catch (const Error &) {
      throw AccessDenied("server asset lock claim is stale or expired");
    }
  }
  return [now, deadline](const ProgressEvent &event) {
    if (event.phase == ProgressPhase::publishing) {
      const auto current = unix_seconds();
      if (current < now || current >= deadline)
        throw AccessDenied("server asset lock expired before publication");
    }
  };
}
} // namespace syrnck::net::detail
