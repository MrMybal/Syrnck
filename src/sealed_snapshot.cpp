#include "sealed_internal.hpp"
#include <blake3.h>
#include <algorithm>
#include <sstream>

namespace syrnck {
void SealedLimits::validate() const {
  require(max_objects >= 1 && max_objects <= 4194304, "sealed object limit must be in 1..4194304");
  require(max_work >= 1 && max_work <= 1073741824, "sealed work limit must be in 1..1073741824");
  require(max_index_cache_bytes >= 1024 && max_index_cache_bytes <= 256 * 1024 * 1024,
          "sealed index cache limit must be in 1024..268435456");
  require(max_new_index_bytes >= 1024 && max_new_index_bytes <= 1024 * 1024 * 1024,
          "sealed new index limit must be in 1024..1073741824");
  require(max_history >= 1 && max_history <= 1000000, "sealed history limit must be in 1..1000000");
}
namespace sealed_detail {
void channel_name(std::string_view channel) {
  validate_ref(channel);
  require(channel.size() <= 64 && channel.find('/') == channel.npos,
          "sealed channel must be one component of at most 64 bytes");
}
namespace {
fs::path resolved_location(const fs::path &input) {
  auto existing = fs::absolute(input);
  fs::path suffix;
  for (;;) {
    std::error_code ec;
    const auto status = fs::status(existing, ec);
    if (fs::exists(status))
      return suffix.empty() ? fs::canonical(existing) : fs::canonical(existing) / suffix;
    if (ec && ec != std::errc::no_such_file_or_directory)
      throw fs::filesystem_error("sealed path", existing, ec);
    const auto parent = existing.parent_path();
    require(!parent.empty() && parent != existing && existing.filename() != "." &&
                existing.filename() != "..",
            "invalid sealed wire location");
    suffix = suffix.empty() ? existing.filename() : existing.filename() / suffix;
    existing = parent;
  }
}
void distinct_paths(const fs::path &left, const fs::path &right) {
  const auto a = resolved_location(left), b = resolved_location(right);
  auto contains = [](const fs::path &parent, fs::path child) {
    const auto minimum = std::distance(parent.begin(), parent.end());
    auto depth = std::distance(child.begin(), child.end());
    while (depth >= minimum) {
      if (fs::exists(parent) && fs::exists(child) && fs::equivalent(parent, child))
        return true;
      const auto next = child.parent_path();
      if (next == child || next.empty())
        return false;
      child = next;
      --depth;
    }
    return false;
  };
  require(!contains(a, b) && !contains(b, a), "sealed wire and plaintext stores must not overlap");
}
} // namespace
void validate_wire_location(const Store &plaintext, const fs::path &wire, OperationOptions options) {
  distinct_paths(plaintext.root(), wire);
  // A conventional project Store also identifies its enclosing project before
  // the first checkout has bound an explicit native workspace directory.
  const auto conventional = plaintext.root().parent_path() / ".syrnck";
  if (fs::exists(conventional) && fs::equivalent(plaintext.root(), conventional))
    distinct_paths(plaintext.root().parent_path(), wire);
  const auto workspaces = plaintext.native_workspaces(options);
  for (const auto &row : workspaces["workspaces"]) {
    throw_if_cancelled(options.stop);
    if (!row["directory"].is_null())
      distinct_paths(path_from_utf8(row["directory"].get<std::string>()), wire);
  }
}
void distinct_stores(const Store &plaintext, const Store &wire) {
  validate_wire_location(plaintext, wire.root());
}
Bytes read_file(const Store &wire, const std::string &manifest, std::size_t maximum, Budget &budget) {
  budget.work();
  const auto metadata = wire.metadata(Kind::manifest, manifest);
  require(metadata["size"].get<std::uint64_t>() <= maximum, "sealed ciphertext file exceeds its limit");
  std::ostringstream output(std::ios::binary);
  wire.reconstruct(manifest, output, budget.options);
  const auto data = output.str();
  return Bytes(data.begin(), data.end());
}
namespace {
Json carrier_stamp(const Json &value) {
  Json stamp{{"version", value["version"]}, {"timestamp", value["timestamp"]}};
  if (value["version"] == 2) {
    stamp["author"] = value["author"];
    stamp["committer"] = value["committer"];
    auto signature = value["signature"];
    if (!signature.is_null())
      signature.erase("value");
    stamp["signature"] = std::move(signature);
  }
  return stamp;
}
Json root_document(const Cipher &cipher, std::string_view channel, const Refs &roots,
                   const std::optional<std::string> &previous, const std::string &index, const Json &stamp) {
  return Json{{"version", 1},
              {"type", "syrnck-sealed-roots"},
              {"realm", cipher.realm},
              {"channel", channel},
              {"previous", previous ? Json(*previous) : Json{}},
              {"index", index},
              {"carrier", stamp},
              {"roots", encode_refs(roots)}};
}
void root_count(const Refs &roots) {
  require(!roots.empty() && roots.size() <= 100000, "sealed snapshot requires 1..100000 roots");
}
template <class Loader> std::map<ObjectKey, bool> audit(const Refs &roots, Budget &budget, Loader load) {
  root_count(roots);
  std::map<ObjectKey, bool> known;
  std::vector<ObjectKey> pending;
  auto enqueue = [&](const ObjectKey &key) {
    budget.work();
    if (known.contains(key))
      return;
    require(known.size() < budget.limits.max_objects, "sealed object limit exceeded");
    known.emplace(key, false);
    pending.push_back(key);
  };
  for (const auto &[name, key] : roots) {
    validate_ref(name);
    validate_id(key.id);
    enqueue(key);
  }
  while (!pending.empty()) {
    budget.work();
    const auto key = std::move(pending.back());
    pending.pop_back();
    if (known.at(key))
      continue;
    const auto data = load(key);
    known.at(key) = true;
    for (const auto &child : dependencies(key.kind, data))
      enqueue(child);
    if (key.kind == Kind::manifest) {
      const auto manifest = parse_json(data);
      blake3_hasher whole;
      blake3_hasher_init(&whole);
      for (const auto &entry : manifest["blocks"]) {
        budget.work();
        const ObjectKey block{Kind::chunk, entry["id"].get<std::string>()};
        const auto payload = load(block);
        known.at(block) = true;
        require(payload.size() == entry["size"].get<std::uint64_t>(), "sealed file block size mismatch");
        blake3_hasher_update(&whole, payload.data(), payload.size());
      }
      Key digest{};
      blake3_hasher_finalize(&whole, digest.data(), digest.size());
      require(hex(digest) == manifest["file_hash"].get<std::string>(), "sealed file hash mismatch");
    }
    if (known.size() % 256 == 0)
      report_progress(budget.options, {ProgressPhase::verifying, key.id, known.size()});
  }
  return known;
}
} // namespace
std::map<ObjectKey, bool> audit_source(const Store &source, const Refs &roots, Budget &budget) {
  return audit(roots, budget, [&](const ObjectKey &key) { return source.get(key); });
}
Root read_root(const Store &wire, const Cipher &cipher, std::string_view snapshot, std::string_view channel,
               Budget &budget) {
  budget.work();
  validate_id(snapshot);
  channel_name(channel);
  const auto value = wire.metadata(Kind::revision, snapshot);
  require(value["message"].get<std::string>() == root_message && value["files"].size() == 1 &&
              value["files"].contains(std::string(root_path)),
          "invalid sealed snapshot carrier");
  const auto &file = value["files"][std::string(root_path)];
  require(file["executable"] == false, "invalid sealed snapshot control entry");
  const auto encoded =
      read_file(wire, file["manifest"].get<std::string>(), max_object_size + frame_overhead, budget);
  const auto plain = cipher.open("roots", channel, encoded, max_object_size);
  const auto document = parse_unique_json(plain);
  require(document.is_object() && document.size() == 8 && document.contains("version") &&
              document.contains("type") && document.contains("realm") && document.contains("channel") &&
              document.contains("previous") && document.contains("index") && document.contains("carrier") &&
              document.contains("roots"),
          "invalid sealed root document");
  require(document["version"] == 1 && document["type"] == "syrnck-sealed-roots" &&
              document["realm"] == cipher.realm && document["channel"].get<std::string>() == channel,
          "sealed root context mismatch");
  Root result{std::string(snapshot),
              document["index"].get<std::string>(),
              {},
              parse_refs(bytes(document["roots"].dump()), "invalid sealed root references")};
  root_count(result.roots);
  validate_id(result.index);
  Json parents = Json::array();
  if (!document["previous"].is_null()) {
    result.previous = document["previous"].get<std::string>();
    validate_id(*result.previous);
    require(*result.previous != snapshot && *result.previous != result.index,
            "invalid sealed snapshot parent");
    parents.push_back(*result.previous);
  }
  parents.push_back(result.index);
  require(value["parents"] == parents, "sealed snapshot parent commitments changed");
  require(root_document(cipher, channel, result.roots, result.previous, result.index, carrier_stamp(value))
                  .dump() == text(plain),
          "noncanonical sealed root document");
  return result;
}
std::optional<Root> read_checkpoint(const Store &store, const Cipher &cipher, std::string_view channel,
                                    std::string_view checkpoint, const Refs &refs) {
  validate_ref(checkpoint);
  require(checkpoint.starts_with("sealed/checkpoints/") && checkpoint.size() > 19,
          "sealed checkpoint must be under sealed/checkpoints/");
  const auto found = refs.find(std::string(checkpoint));
  if (found == refs.end())
    return {};
  require(found->second.kind == Kind::chunk, "invalid sealed checkpoint reference");
  const auto raw = store.get(found->second);
  const auto value = parse_unique_json(raw);
  require(value.is_object() && value.size() == 6 && value.value("version", 0) == 1 &&
              value.value("type", "") == "syrnck-sealed-checkpoint" &&
              value.value("realm", "") == cipher.realm && value.value("channel", "") == channel &&
              value.contains("snapshot") && value.contains("roots"),
          "sealed checkpoint context mismatch");
  Root root{value["snapshot"].get<std::string>(),
            {},
            {},
            parse_refs(bytes(value["roots"].dump()), "invalid sealed checkpoint roots")};
  validate_id(root.id);
  root_count(root.roots);
  const auto canonical = Json{{"version", 1},          {"type", "syrnck-sealed-checkpoint"},
                              {"realm", cipher.realm}, {"channel", channel},
                              {"snapshot", root.id},   {"roots", encode_refs(root.roots)}}
                             .dump();
  require(canonical == text(raw), "noncanonical sealed checkpoint");
  return root;
}
bool descends_from(const Store &wire, const Cipher &cipher, const Root &root, std::string_view ancestor,
                   std::string_view channel, Budget &budget) {
  auto at = root;
  std::size_t history = 0;
  while (at.id != ancestor) {
    if (!at.previous)
      return false;
    require(++history <= budget.limits.max_history, "sealed history limit exceeded");
    at = read_root(wire, cipher, *at.previous, channel, budget);
  }
  return true;
}
namespace {
Bytes original_object(const Store &wire, const Cipher &cipher, Index &index, const Root &root,
                      const ObjectKey &key, Budget &budget) {
  budget.work();
  const auto address = cipher.address(key);
  const auto manifest = index.find(root.index, address);
  require(manifest.has_value(), "sealed snapshot is missing an original object");
  const auto frame = read_file(wire, *manifest, max_object_size + frame_overhead + 33, budget);
  const auto plain = cipher.open("object", address, frame, max_object_size + 33);
  require(plain.size() > 33 && plain[0] == static_cast<std::uint8_t>(key.kind) &&
              hex(std::span(plain).subspan(1, 32)) == key.id,
          "sealed original object identity mismatch");
  const auto data = std::span(plain).subspan(33);
  validate_object(key.kind, data);
  require(object_id(key.kind, data) == key.id, "sealed original object hash mismatch");
  return Bytes(data.begin(), data.end());
}
Json report(const Root &root, const Cipher &cipher, const Budget &budget, std::string_view channel,
            std::size_t objects) {
  return Json{{"version", 1},
              {"realm", cipher.realm},
              {"channel", channel},
              {"snapshot", root.id},
              {"index", root.index},
              {"roots", encode_refs(root.roots)},
              {"objects", objects},
              {"index_nodes_read", budget.index_reads},
              {"index_nodes_written", budget.index_writes},
              {"work", budget.used}};
}
} // namespace
} // namespace sealed_detail

Json seal_snapshot(const Store &source, const Store &wire, const SecretKey &secret, std::string_view channel,
                   const Refs &roots, std::optional<std::string> previous, const SealedLimits &limits,
                   OperationOptions options) {
  using namespace sealed_detail;
  limits.validate();
  channel_name(channel);
  distinct_stores(source, wire);
  Budget budget{limits, options};
  Cipher cipher(secret);
  std::optional<Root> old;
  Index index(wire, budget);
  if (previous) {
    old = read_root(wire, cipher, *previous, channel, budget);
    (void)audit(old->roots, budget, [&](const ObjectKey &key) {
      return original_object(wire, cipher, index, *old, key, budget);
    });
  }
  require(encode_refs(roots).dump().size() <= max_object_size - 4096, "sealed root document exceeds 16 MiB");
  // Validate complete immutable plaintext inputs before creating ciphertext.
  const auto objects = audit_source(source, roots, budget);
  if (old && old->roots == roots) {
    auto result = report(*old, cipher, budget, channel, objects.size());
    result["state"] = "unchanged";
    result["sealed_objects"] = 0;
    result["reused_objects"] = objects.size();
    result["retained_ref"] = Json{};
    return result;
  }
  // Refuse an unavailable required signer before writing any payloads.
  const auto clock = unix_seconds();
  const auto prototype = source.native_commit_metadata(Json{{"version", 1},
                                                            {"parents", Json::array()},
                                                            {"message", root_message},
                                                            {"timestamp", clock},
                                                            {"files", Json::object()}});
  struct Item {
    std::string address;
    const ObjectKey *key;
    std::optional<std::string> existing;
  };
  std::vector<Item> ordered;
  ordered.reserve(objects.size());
  for (const auto &[key, visited] : objects) {
    (void)visited;
    budget.work();
    ordered.push_back({cipher.address(key), &key, {}});
  }
  std::sort(ordered.begin(), ordered.end(),
            [](const auto &a, const auto &b) { return a.address < b.address; });
  std::size_t index_bytes = 0, count = 0;
  for (std::size_t i = 0; i < ordered.size(); ++i) {
    budget.work();
    auto &item = ordered[i];
    require(i == 0 || ordered[i - 1].address != item.address, "duplicate sealed object address");
    if (old)
      item.existing = index.find(old->index, item.address);
    if (!item.existing) {
      require(192 <= limits.max_new_index_bytes - index_bytes, "sealed new index limit exceeded");
      index_bytes += 192;
      ++count;
    }
  }
  std::vector<Index::Change> changes;
  changes.reserve(count);
  std::uint64_t encrypted_bytes = 0;
  for (const auto &item : ordered) {
    budget.work();
    if (item.existing)
      continue;
    const auto data = source.get(*item.key);
    auto identifier = unhex(item.key->id);
    Bytes plain{static_cast<std::uint8_t>(item.key->kind)};
    plain.insert(plain.end(), identifier.begin(), identifier.end());
    plain.insert(plain.end(), data.begin(), data.end());
    const auto frame = cipher.seal("object", item.address, plain);
    std::istringstream input(text(frame), std::ios::binary);
    const auto manifest = wire.ingest(input, {}, {.stop = options.stop, .progress = options.progress}).first;
    changes.push_back({item.address, manifest});
    encrypted_bytes += data.size();
    report_progress(options, {ProgressPhase::ingesting, {}, changes.size(), count});
  }
  const auto tree = index.update(source, old ? std::optional{old->index} : std::nullopt, changes);
  const auto control = root_document(cipher, channel, roots, previous, tree, carrier_stamp(prototype)).dump();
  require(control.size() <= max_object_size, "sealed root document exceeds 16 MiB");
  const auto frame = cipher.seal("roots", channel, bytes(control));
  std::istringstream input(text(frame), std::ios::binary);
  const auto manifest = wire.ingest(input, {}, {.stop = options.stop, .progress = options.progress}).first;
  Json parents = Json::array();
  if (previous)
    parents.push_back(*previous);
  parents.push_back(tree);
  const auto draft = source.native_commit_metadata(
      Json{{"version", 1},
           {"parents", std::move(parents)},
           {"message", root_message},
           {"timestamp", clock},
           {"files", Json{{std::string(root_path), Json{{"manifest", manifest}, {"executable", false}}}}}});
  const auto root = wire.put(Kind::revision, bytes(draft.dump())).first;
  const auto retained = "sealed/prepared/" + root.id;
  wire.set_ref(retained, root, options.stop);
  auto result = report(Root{root.id, tree, previous, roots}, cipher, budget, channel, objects.size());
  result["state"] = "prepared";
  result["retained_ref"] = retained;
  result["sealed_objects"] = changes.size();
  result["reused_objects"] = objects.size() - changes.size();
  result["plaintext_bytes_sealed"] = encrypted_bytes;
  report_progress(options, {ProgressPhase::planning, {}, objects.size(), objects.size(), true});
  return result;
}
Json inspect_sealed_snapshot(const Store &wire, const SecretKey &secret, std::string_view snapshot,
                             std::string_view channel, const SealedLimits &limits, OperationOptions options) {
  using namespace sealed_detail;
  limits.validate();
  Budget budget{limits, options};
  Cipher cipher(secret);
  const auto root = read_root(wire, cipher, snapshot, channel, budget);
  Index index(wire, budget);
  const auto objects = audit(root.roots, budget, [&](const ObjectKey &key) {
    return original_object(wire, cipher, index, root, key, budget);
  });
  auto result = report(root, cipher, budget, channel, objects.size());
  result["state"] = "verified";
  report_progress(options, {ProgressPhase::verifying, {}, objects.size(), objects.size(), true});
  return result;
}
Json open_sealed_snapshot(const Store &wire, const Store &destination, const SecretKey &secret,
                          std::string_view snapshot, std::string_view channel, std::string_view checkpoint,
                          const SealedLimits &limits, OperationOptions options) {
  using namespace sealed_detail;
  limits.validate();
  channel_name(channel);
  distinct_stores(destination, wire);
  validate_ref(checkpoint);
  require(checkpoint.starts_with("sealed/checkpoints/") && checkpoint.size() > 19,
          "sealed checkpoint must be under sealed/checkpoints/");
  require_no_native_checkout(destination);
  Budget budget{limits, options};
  Cipher cipher(secret);
  const auto root = read_root(wire, cipher, snapshot, channel, budget);
  const auto refs = destination.refs();
  const auto found = refs.find(std::string(checkpoint));
  std::optional<ObjectKey> expected;
  if (const auto old = read_checkpoint(destination, cipher, channel, checkpoint, refs)) {
    expected = found->second;
    require(descends_from(wire, cipher, root, old->id, channel, budget),
            "sealed snapshot rolls back or forks the checkpoint");
  }
  Index index(wire, budget);
  std::size_t imported = 0;
  const auto objects = audit(root.roots, budget, [&](const ObjectKey &key) {
    const auto plain = original_object(wire, cipher, index, root, key, budget);
    if (!destination.contains(key)) {
      const auto result = destination.put(key.kind, plain).first;
      require(result == key, "sealed import object identity changed");
      ++imported;
    }
    return plain;
  });
  std::vector<RefUpdate> updates;
  for (const auto &[name, key] : root.roots) {
    const auto retained = "sealed/roots/" + root.id + '/' + hash(bytes(name));
    const auto at = refs.find(retained);
    const auto before = at == refs.end() ? std::optional<ObjectKey>{} : std::optional{at->second};
    require(!before || *before == key, "sealed snapshot retention reference is occupied");
    updates.push_back({retained, before, key});
  }
  const auto receipt = Json{{"version", 1},          {"type", "syrnck-sealed-checkpoint"},
                            {"realm", cipher.realm}, {"channel", channel},
                            {"snapshot", root.id},   {"roots", encode_refs(root.roots)}}
                           .dump();
  require(receipt.size() <= max_object_size, "sealed checkpoint exceeds 16 MiB");
  const auto next = destination.put(Kind::chunk, bytes(receipt)).first;
  updates.push_back({std::string(checkpoint), expected, next});
  if (expected && *expected != next) {
    const auto archive = "archive/sealed/" + expected->id;
    const auto at = refs.find(archive);
    const auto before = at == refs.end() ? std::optional<ObjectKey>{} : std::optional{at->second};
    require(!before || *before == *expected, "sealed checkpoint archive reference is occupied");
    updates.push_back({archive, before, expected});
  }
  require(destination.compare_update_refs(updates, options), "sealed checkpoint changed; reload and retry");
  auto result = report(root, cipher, budget, channel, objects.size());
  result["state"] = "opened";
  result["checkpoint"] = checkpoint;
  result["imported_objects"] = imported;
  report_progress(options, {ProgressPhase::planning, {}, objects.size(), objects.size(), true});
  return result;
}
} // namespace syrnck
