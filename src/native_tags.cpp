#include "native_tag_archive.hpp"
#include <algorithm>
namespace syrnck {
namespace {
constexpr std::string_view tag_prefix = "tags/", annotation_prefix = "tag-metadata/";
std::optional<ObjectKey> lookup(const Refs &refs, const std::string &name) {
  const auto found = refs.find(name);
  return found == refs.end() ? std::optional<ObjectKey>{} : std::optional{found->second};
}
void tag_name(std::string_view name) {
  require(!name.starts_with('-'), "native tag name cannot start with '-'");
  validate_ref(name);
  validate_ref(std::string(tag_prefix) + std::string(name));
  validate_ref(std::string(annotation_prefix) + std::string(name));
}
struct Tag {
  std::string name, revision;
  std::optional<ObjectKey> annotation;
  std::optional<std::string> message;
  std::uint64_t timestamp = 0;
  ObjectKey target() const {
    return {Kind::revision, revision};
  }
  std::string ref() const {
    return std::string(tag_prefix) + name;
  }
  std::string metadata_ref() const {
    return std::string(annotation_prefix) + name;
  }
  Json metadata() const {
    return Json{{"version", 1},         {"type", "native-tag"}, {"name", name},
                {"revision", revision}, {"message", *message},  {"timestamp", timestamp}};
  }
  Json json() const {
    return Json{{"name", name},
                {"revision", revision},
                {"annotation", annotation ? Json(annotation->id) : Json{}},
                {"message", message ? Json(*message) : Json{}},
                {"timestamp", message ? Json(timestamp) : Json{}}};
  }
};
void read_annotation(const Store &store, Tag &tag) {
  if (!tag.annotation)
    return;
  require(tag.annotation->kind == Kind::chunk, "invalid native tag annotation");
  const auto data = store.get(*tag.annotation);
  const auto value = parse_json(data);
  require(value.is_object() && value.size() == 6 && value.value("version", 0) == 1 &&
              value.value("type", "") == "native-tag" && value.at("name") == tag.name &&
              value.at("revision") == tag.revision && value.at("message").is_string() &&
              value.at("timestamp").is_number_unsigned(),
          "invalid native tag annotation");
  tag.message = value.at("message").get<std::string>();
  tag.timestamp = value.at("timestamp").get<std::uint64_t>();
  require(tag.message->size() <= 65536 && tag.metadata().dump() == text(data),
          "invalid native tag annotation");
}
Tag read_tag(const Store &store, const Refs &refs, std::string_view name) {
  tag_name(name);
  Tag tag;
  tag.name = name;
  const auto target = lookup(refs, tag.ref());
  require(target.has_value(), "unknown native tag");
  require(target->kind == Kind::revision, "native tag must point to a revision");
  tag.revision = target->id;
  tag.annotation = lookup(refs, tag.metadata_ref());
  read_annotation(store, tag);
  return tag;
}
Json report(const Tag &tag, bool verified) {
  return Json{{"version", 1}, {"tag", tag.json()}, {"objects_verified", verified}};
}
struct Archive {
  Tag tag;
  std::string nonce;
  std::uint64_t timestamp = 0;
  ObjectKey record{Kind::chunk, {}};
  std::string prefix() const {
    return "archive/tags/" + record.id;
  }
  Refs roots() const {
    Refs result{{prefix() + "/record", record}, {prefix() + "/target", tag.target()}};
    if (tag.annotation)
      result.emplace(prefix() + "/annotation", *tag.annotation);
    return result;
  }
  Json json() const {
    return Json{{"version", 1},
                {"type", "native-tag-archive"},
                {"name", tag.name},
                {"revision", tag.revision},
                {"annotation", tag.annotation ? Json(tag.annotation->id) : Json{}},
                {"nonce", nonce},
                {"timestamp", timestamp}};
  }
  Json report(bool verified) const {
    return Json{{"version", 1},
                {"archive", record.id},
                {"tag", tag.json()},
                {"deleted_at", timestamp},
                {"objects_verified", verified}};
  }
};
Archive read_archive(const Store &store, const Refs &refs, std::string_view id) {
  validate_id(id);
  Archive archive;
  archive.record.id = id;
  require(lookup(refs, archive.prefix() + "/record") == archive.record, "unknown native tag archive");
  const auto data = store.get(archive.record);
  const auto value = parse_json(data);
  require(value.is_object() && value.size() == 7 && value.value("version", 0) == 1 &&
              value.value("type", "") == "native-tag-archive" && value.at("timestamp").is_number_unsigned(),
          "invalid native tag archive");
  archive.tag.name = value.at("name").get<std::string>();
  tag_name(archive.tag.name);
  archive.tag.revision = value.at("revision").get<std::string>();
  validate_id(archive.tag.revision);
  if (!value.at("annotation").is_null()) {
    const auto annotation = value.at("annotation").get<std::string>();
    validate_id(annotation);
    archive.tag.annotation = ObjectKey{Kind::chunk, annotation};
  }
  archive.nonce = value.at("nonce").get<std::string>();
  validate_id(archive.nonce);
  archive.timestamp = value.at("timestamp").get<std::uint64_t>();
  require(archive.json().dump() == text(data) &&
              lookup(refs, archive.prefix() + "/target") == archive.tag.target() &&
              lookup(refs, archive.prefix() + "/annotation") == archive.tag.annotation,
          "invalid native tag archive");
  read_annotation(store, archive.tag);
  const auto wanted = archive.roots();
  const auto prefix = archive.prefix() + '/';
  std::size_t count = 0;
  for (auto it = refs.lower_bound(prefix); it != refs.end() && it->first.starts_with(prefix); ++it) {
    const auto expected = wanted.find(it->first);
    require(expected != wanted.end() && expected->second == it->second, "invalid native tag archive");
    ++count;
  }
  require(count == wanted.size(), "invalid native tag archive");
  return archive;
}
} // namespace
NativeTagArchiveRoots read_native_tag_archive_roots(const Store &store, const Refs &refs, std::string_view id,
                                                    OperationOptions options) {
  throw_if_cancelled(options.stop);
  const auto archive = read_archive(store, refs, id);
  return {archive.tag.name, archive.tag.target(), archive.roots()};
}
Json Store::native_tag_archives(const std::optional<std::string> &after, std::size_t limit,
                                bool verify_objects, OperationOptions options) const {
  throw_if_cancelled(options.stop);
  require(limit >= 1 && limit <= 1000, "native tag page limit must be in 1..1000");
  if (after)
    validate_id(*after);
  const auto all = refs();
  const std::string prefix = "archive/tags/";
  // IDs have exactly 64 hex characters; '0' sorts after the '/' member group.
  auto it = all.lower_bound(after ? prefix + *after + '0' : prefix);
  Json rows = Json::array();
  std::vector<ObjectKey> roots;
  std::size_t budget = 4096;
  for (; it != all.end() && it->first.starts_with(prefix) && rows.size() < limit;) {
    throw_if_cancelled(options.stop);
    const auto rest = it->first.substr(prefix.size());
    const auto slash = rest.find('/');
    require(slash != rest.npos, "invalid native tag archive");
    const auto archive = read_archive(*this, all, rest.substr(0, slash));
    auto row = archive.report(false);
    row.erase("version");
    row.erase("objects_verified");
    const auto size = row.dump().size() + 1;
    if (size > max_object_size - budget)
      break;
    budget += size;
    rows.push_back(std::move(row));
    for (const auto &[name, key] : archive.roots()) {
      (void)name;
      roots.push_back(key);
    }
    const auto group = archive.prefix() + '/';
    do {
      ++it;
    } while (it != all.end() && it->first.starts_with(group));
    report_progress(options, {ProgressPhase::planning, archive.record.id, rows.size()});
  }
  if (verify_objects)
    (void)closure(roots, true, options.stop, options.progress);
  const bool more = it != all.end() && it->first.starts_with(prefix);
  const auto next = more && !rows.empty() ? rows.back()["archive"] : Json{};
  report_progress(options, {ProgressPhase::planning, {}, rows.size(), rows.size(), true});
  return Json{{"version", 1},
              {"archives", std::move(rows)},
              {"has_more", more},
              {"next_after", next},
              {"objects_verified", verify_objects}};
}
Json Store::native_tags(const std::optional<std::string> &after, std::size_t limit,
                        OperationOptions options) const {
  throw_if_cancelled(options.stop);
  require(limit >= 1 && limit <= 1000, "native tag page limit must be in 1..1000");
  if (after)
    tag_name(*after);
  const auto all = refs();
  // Orphan notes can indicate an incomplete manual edit. Never silently adopt
  // them when a new tag with the same name is created.
  for (auto it = all.lower_bound(std::string(annotation_prefix));
       it != all.end() && it->first.starts_with(annotation_prefix); ++it) {
    throw_if_cancelled(options.stop);
    require(all.contains(std::string(tag_prefix) + it->first.substr(annotation_prefix.size())),
            "native tag annotation has no tag");
  }
  Json tags = Json::array();
  std::size_t budget = 4096;
  auto it =
      after ? all.upper_bound(std::string(tag_prefix) + *after) : all.lower_bound(std::string(tag_prefix));
  for (; it != all.end() && it->first.starts_with(tag_prefix) && tags.size() < limit; ++it) {
    throw_if_cancelled(options.stop);
    const auto tag = read_tag(*this, all, it->first.substr(tag_prefix.size()));
    auto entry = tag.json();
    const auto size = entry.dump().size() + 1;
    if (size > max_object_size - budget)
      break;
    budget += size;
    tags.push_back(std::move(entry));
    report_progress(options, {ProgressPhase::planning, tag.name, tags.size()});
  }
  report_progress(options, {ProgressPhase::planning, {}, tags.size(), tags.size(), true});
  const bool more = it != all.end() && it->first.starts_with(tag_prefix);
  const auto next = more && !tags.empty() ? tags.back()["name"] : Json{};
  return Json{{"version", 1}, {"tags", std::move(tags)}, {"has_more", more}, {"next_after", next}};
}
Json Store::native_tag(std::string_view name, bool verify_objects, OperationOptions options) const {
  throw_if_cancelled(options.stop);
  const auto tag = read_tag(*this, refs(), name);
  if (verify_objects)
    (void)closure({tag.target()}, true, options.stop, options.progress);
  throw_if_cancelled(options.stop);
  return report(tag, verify_objects);
}
Json Store::create_native_tag(std::string_view name, std::string_view revision,
                              const std::optional<std::string> &message, OperationOptions options) const {
  throw_if_cancelled(options.stop);
  tag_name(name);
  validate_id(revision);
  Tag tag;
  tag.name = name;
  tag.revision = revision;
  tag.message = message;
  const auto all = refs();
  require(!all.contains(tag.ref()) && !all.contains(tag.metadata_ref()), "native tag already exists");
  if (message) {
    require(message->size() <= 65536, "native tag message exceeds 64 KiB");
    tag.timestamp = unix_seconds();
    tag.annotation = put(Kind::chunk, bytes(tag.metadata().dump())).first;
  }
  require(
      compare_update_refs({{tag.ref(), {}, tag.target()}, {tag.metadata_ref(), {}, tag.annotation}}, options),
      "native tag references changed");
  return report(tag, true);
}
Json Store::delete_native_tag(std::string_view name, std::string_view expected_revision,
                              OperationOptions options) const {
  throw_if_cancelled(options.stop);
  validate_id(expected_revision);
  Archive archive;
  archive.tag = read_tag(*this, refs(), name);
  require(archive.tag.revision == expected_revision, "native tag target changed");
  archive.nonce = hex(random_bytes(32));
  archive.timestamp = unix_seconds();
  archive.record = put(Kind::chunk, bytes(archive.json().dump())).first;
  std::vector<RefUpdate> updates{{archive.tag.ref(), archive.tag.target(), {}},
                                 {archive.tag.metadata_ref(), archive.tag.annotation, {}},
                                 {archive.prefix() + "/record", {}, archive.record},
                                 {archive.prefix() + "/target", {}, archive.tag.target()}};
  if (archive.tag.annotation)
    updates.push_back({archive.prefix() + "/annotation", {}, archive.tag.annotation});
  require(compare_update_refs(updates, options), "native tag references changed");
  return archive.report(true);
}
Json Store::native_tag_archive(std::string_view id, bool verify_objects, OperationOptions options) const {
  throw_if_cancelled(options.stop);
  const auto archive = read_archive(*this, refs(), id);
  if (verify_objects)
    (void)closure({archive.tag.target()}, true, options.stop, options.progress);
  throw_if_cancelled(options.stop);
  return archive.report(verify_objects);
}
Json Store::restore_native_tag(std::string_view archive_id, OperationOptions options) const {
  throw_if_cancelled(options.stop);
  const auto archive = read_archive(*this, refs(), archive_id);
  const auto &tag = archive.tag;
  const auto all = refs();
  require(!all.contains(tag.ref()) && !all.contains(tag.metadata_ref()), "native tag already exists");
  require(compare_update_refs({{tag.ref(), {}, tag.target()},
                               {tag.metadata_ref(), {}, tag.annotation},
                               {archive.prefix() + "/record", archive.record, archive.record},
                               {archive.prefix() + "/target", tag.target(), tag.target()},
                               {archive.prefix() + "/annotation", tag.annotation, tag.annotation}},
                              options),
          "native tag references changed");
  return report(tag, true);
}
std::string Store::resolve_native_revision(std::string_view selector, OperationOptions options) const {
  throw_if_cancelled(options.stop);
  std::optional<ObjectKey> target;
  if (selector.size() == 64 && std::all_of(selector.begin(), selector.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
      }))
    target = ObjectKey{Kind::revision, std::string(selector)};
  else if (selector == "HEAD") {
    const auto head = native_head();
    require(!head["revision"].is_null(), "native HEAD is unborn");
    target = ObjectKey{Kind::revision, head["revision"].get<std::string>()};
  } else if (native_target_is_detached(selector)) {
    target = lookup(refs(), native_revision_reference(*this, selector));
  } else {
    validate_ref(selector);
    const auto all = refs();
    if (selector.starts_with(tag_prefix))
      target = read_tag(*this, all, selector.substr(tag_prefix.size())).target();
    else if (selector.starts_with("heads/") || selector.starts_with("remotes/"))
      target = lookup(all, std::string(selector));
    else {
      const auto branch = lookup(all, "heads/" + std::string(selector));
      const auto tag = lookup(all, std::string(tag_prefix) + std::string(selector));
      require(!(branch && tag), "ambiguous native revision; use heads/NAME or tags/NAME");
      target = tag ? std::optional{read_tag(*this, all, selector).target()} : branch;
    }
  }
  require(target.has_value(), "unknown native revision");
  require(target->kind == Kind::revision, "native revision reference must point to a revision");
  (void)metadata(Kind::revision, target->id);
  throw_if_cancelled(options.stop);
  return target->id;
}
} // namespace syrnck
