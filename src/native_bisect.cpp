#include "internal.hpp"
#include <syrnck/bisect.hpp>
#include <algorithm>
#include <bit>
#include <deque>
namespace syrnck {
namespace {
constexpr std::string_view active_name = "bisect/active", pin_name = "bisect/pin";
constexpr std::size_t max_candidates = 4096, max_history = 100000, max_links = 1000000;
std::optional<ObjectKey> lookup(const Refs &refs, std::string_view name) {
  const auto it = refs.find(std::string(name));
  return it == refs.end() ? std::optional<ObjectKey>{} : std::optional{it->second};
}
struct Record {
  std::string session, seed_good, seed_bad, bad;
  std::set<std::string> good, skipped;
  ObjectKey key{Kind::chunk, {}};
  Json json() const {
    return Json{{"version", 1},           {"type", "native-bisect"}, {"session", session},
                {"seed_good", seed_good}, {"seed_bad", seed_bad},    {"bad", bad},
                {"good", good},           {"skipped", skipped}};
  }
};
std::set<std::string> identifiers(const Json &value) {
  require(value.is_array() && value.size() <= max_candidates, "invalid native bisect revision list");
  std::set<std::string> result;
  for (const auto &entry : value) {
    require(entry.is_string(), "invalid native bisect revision list");
    const auto id = entry.get<std::string>();
    validate_id(id);
    require(result.insert(id).second, "invalid native bisect revision list");
  }
  return result;
}
std::optional<Record> read(const Store &store, const Refs &refs) {
  const auto key = lookup(refs, store.native_workspace_reference(active_name)), pin = lookup(refs, store.native_workspace_reference(pin_name));
  if (!key) {
    require(!pin, "native bisect has an orphan history pin");
    return {};
  }
  require(key->kind == Kind::chunk, "invalid native bisect state");
  const auto data = store.get(*key);
  const auto value = parse_json(data);
  require(value.is_object() && value.size() == 8 && value.contains("version") &&
              value["version"].is_number_unsigned() && value["version"] == 1 && value.contains("type") &&
              value["type"] == "native-bisect" && value.contains("good") && value.contains("skipped"),
          "invalid native bisect state");
  Record record;
  for (const auto &[name, destination] :
       std::initializer_list<std::pair<std::string_view, std::string *>>{{"session", &record.session},
                                                                         {"seed_good", &record.seed_good},
                                                                         {"seed_bad", &record.seed_bad},
                                                                         {"bad", &record.bad}}) {
    const auto field = std::string(name);
    require(value.contains(field) && value[field].is_string(), "invalid native bisect state");
    *destination = value[field].get<std::string>();
    validate_id(*destination);
  }
  record.good = identifiers(value["good"]);
  record.skipped = identifiers(value["skipped"]);
  record.key = *key;
  require(record.good.contains(record.seed_good) && !record.good.contains(record.bad) &&
              !record.skipped.contains(record.bad) && pin == ObjectKey{Kind::revision, record.seed_bad} &&
              record.json().dump() == text(data),
          "invalid native bisect state");
  for (const auto &id : record.skipped)
    require(!record.good.contains(id), "invalid native bisect state");
  return record;
}
class History {
  const Store &store_;
  const OperationOptions &options_;
  std::map<std::string, std::vector<std::string>> nodes_;
  std::size_t links_ = 0, bytes_ = 0;

public:
  History(const Store &store, const OperationOptions &options) : store_(store), options_(options) {}
  const std::vector<std::string> &parents(const std::string &id) {
    throw_if_cancelled(options_.stop);
    if (const auto found = nodes_.find(id); found != nodes_.end())
      return found->second;
    require(nodes_.size() < max_history, "native bisect history exceeds 100000 revisions");
    const auto revision = store_.metadata(Kind::revision, id);
    const auto &parents = revision["parents"];
    require(parents.size() <= max_links - links_, "native bisect history exceeds 1000000 parent links");
    const auto cost = 128 + parents.size() * 64;
    require(cost <= 64 * 1024 * 1024 - bytes_, "native bisect history exceeds 64 MiB metadata budget");
    bytes_ += cost;
    links_ += parents.size();
    auto &result = nodes_[id];
    for (const auto &parent : parents)
      result.push_back(parent.get<std::string>());
    if (nodes_.size() % 256 == 0)
      report_progress(options_, {ProgressPhase::planning, id, nodes_.size()});
    return result;
  }
  std::set<std::string> ancestors(const std::set<std::string> &roots,
                                  const std::set<std::string> &stop = {}) {
    std::vector<std::string> pending(roots.begin(), roots.end());
    std::set<std::string> seen;
    while (!pending.empty()) {
      throw_if_cancelled(options_.stop);
      auto id = std::move(pending.back());
      pending.pop_back();
      if (stop.contains(id) || !seen.insert(id).second)
        continue;
      const auto &edges = parents(id);
      pending.insert(pending.end(), edges.begin(), edges.end());
    }
    return seen;
  }
};
Json analyse(const Store &store, const Record &record, OperationOptions options) {
  History history(store, options);
  const auto initial = history.ancestors({record.seed_bad});
  require(initial.contains(record.seed_good) && record.seed_good != record.seed_bad &&
              initial.contains(record.bad),
          "native bisect requires a good ancestor of the initial bad revision");
  for (const auto &id : record.good)
    require(initial.contains(id), "native bisect result is outside its initial history");
  for (const auto &id : record.skipped)
    require(initial.contains(id), "native bisect result is outside its initial history");
  const auto known_good = history.ancestors(record.good);
  require(!known_good.contains(record.bad), "native bisect good and bad results contradict");
  const auto candidates = history.ancestors({record.bad}, known_good);
  require(!candidates.empty() && candidates.size() <= max_candidates,
          "native bisect exceeds 4096 candidate revisions");
  std::vector<std::string> ids(candidates.begin(), candidates.end());
  std::map<std::string, std::size_t> positions;
  for (std::size_t i = 0; i < ids.size(); ++i)
    positions.emplace(ids[i], i);
  std::vector<std::vector<std::size_t>> parents(ids.size()), children(ids.size());
  std::vector<std::size_t> pending(ids.size());
  std::deque<std::size_t> ready;
  for (std::size_t i = 0; i < ids.size(); ++i) {
    for (const auto &parent : history.parents(ids[i]))
      if (const auto found = positions.find(parent); found != positions.end()) {
        parents[i].push_back(found->second);
        children[found->second].push_back(i);
      }
    pending[i] = parents[i].size();
    if (!pending[i])
      ready.push_back(i);
  }
  const auto words = (ids.size() + 63) / 64;
  std::vector<std::vector<std::uint64_t>> bits(ids.size(), std::vector<std::uint64_t>(words));
  std::vector<std::size_t> counts(ids.size());
  std::size_t processed = 0;
  while (!ready.empty()) {
    throw_if_cancelled(options.stop);
    const auto at = ready.front();
    ready.pop_front();
    for (const auto parent : parents[at])
      for (std::size_t word = 0; word < words; ++word)
        bits[at][word] |= bits[parent][word];
    bits[at][at / 64] |= std::uint64_t{1} << (at % 64);
    for (const auto word : bits[at])
      counts[at] += std::popcount(word);
    for (const auto child : children[at])
      if (--pending[child] == 0)
        ready.push_back(child);
    ++processed;
  }
  require(processed == ids.size(), "native revision graph contains a cycle");
  std::optional<std::string> next;
  std::size_t balance = 0, skipped = 0;
  for (std::size_t i = 0; i < ids.size(); ++i) {
    skipped += record.skipped.contains(ids[i]);
    if (ids[i] == record.bad || record.skipped.contains(ids[i]))
      continue;
    const auto score = std::min(counts[i], ids.size() - counts[i]);
    if (!next || score > balance) {
      next = ids[i];
      balance = score;
    }
  }
  const bool found = ids.size() == 1;
  const auto state = found ? "found" : next ? "testing" : "ambiguous";
  report_progress(options, {ProgressPhase::planning, {}, processed, ids.size(), true});
  return Json{{"version", 1},
              {"active", true},
              {"state", record.key.id.empty() ? Json{} : Json(record.key.id)},
              {"session", record.session},
              {"status", state},
              {"initial_good", record.seed_good},
              {"initial_bad", record.seed_bad},
              {"bad", record.bad},
              {"good", record.good},
              {"skipped", record.skipped},
              {"candidate_count", ids.size()},
              {"skipped_candidates", skipped},
              {"balance", balance},
              {"next_revision", next ? Json(*next) : Json{}},
              {"first_bad", found ? Json(record.bad) : Json{}},
              {"possible_first_bad", !found && !next ? Json(ids) : Json::array()}};
}
} // namespace
Json NativeBisect::start(std::string_view good, std::string_view bad, OperationOptions options) const {
  throw_if_cancelled(options.stop);
  validate_id(good);
  validate_id(bad);
  require_no_native_checkout(store_);
  const auto refs = store_.refs();
  require(!read(store_, refs), "native bisect is already active");
  Record record;
  record.session = hex(random_bytes(32));
  record.seed_good = good;
  record.seed_bad = bad;
  record.bad = bad;
  record.good.insert(std::string(good));
  auto report = analyse(store_, record, options);
  record.key = store_.put(Kind::chunk, bytes(record.json().dump())).first;
  report["state"] = record.key.id;
  require(
      store_.compare_update_refs({{store_.native_workspace_reference(active_name), {}, record.key},
                                  {store_.native_workspace_reference(pin_name), {}, ObjectKey{Kind::revision, record.seed_bad}}},
                                 options),
      "native bisect references changed; retry operation");
  return report;
}
Json NativeBisect::status(OperationOptions options) const {
  throw_if_cancelled(options.stop);
  const auto record = read(store_, store_.refs());
  return record ? analyse(store_, *record, options) : Json{{"version", 1}, {"active", false}};
}
Json NativeBisect::mark(std::string_view result, std::string_view revision, std::string_view expected,
                        OperationOptions options) const {
  throw_if_cancelled(options.stop);
  validate_id(revision);
  validate_id(expected);
  require(result == "good" || result == "bad" || result == "skip", "invalid native bisect result");
  require_no_native_checkout(store_);
  auto record = read(store_, store_.refs());
  require(record.has_value(), "native bisect is not active");
  require(record->key.id == expected, "native bisect state changed; refresh status");
  const auto current = analyse(store_, *record, options);
  require(current["next_revision"].is_string() && current["next_revision"].get<std::string>() == revision,
          "native bisect result must name the suggested revision");
  if (result == "good")
    record->good.insert(std::string(revision));
  else if (result == "bad")
    record->bad = revision;
  else
    record->skipped.insert(std::string(revision));
  auto report = analyse(store_, *record, options);
  const auto next = store_.put(Kind::chunk, bytes(record->json().dump())).first;
  report["state"] = next.id;
  require(store_.compare_update_refs({{store_.native_workspace_reference(active_name), record->key, next}}, options),
          "native bisect references changed; retry operation");
  return report;
}
Json NativeBisect::reset(std::string_view expected, OperationOptions options) const {
  throw_if_cancelled(options.stop);
  validate_id(expected);
  require_no_native_checkout(store_);
  const auto refs = store_.refs();
  const auto record = read(store_, refs);
  require(record.has_value(), "native bisect is not active");
  require(record->key.id == expected, "native bisect state changed; refresh status");
  auto report = analyse(store_, *record, options);
  const ObjectKey root{Kind::revision, record->seed_bad};
  const auto archive = "archive/bisect/" + root.id, receipt = store_.native_workspace_reference("bisect/receipts/" + record->session);
  const auto previous = lookup(refs, archive);
  require(!previous || *previous == root, "native bisect archive reference is occupied");
  require(!lookup(refs, receipt), "native bisect receipt reference is occupied");
  report["active"] = false;
  report["archive"] = archive;
  report["receipt"] = receipt;
  require(store_.compare_update_refs({{store_.native_workspace_reference(active_name), record->key, {}},
                                      {store_.native_workspace_reference(pin_name), root, {}},
                                      {archive, previous, root},
                                      {receipt, {}, record->key}},
                                     options),
          "native bisect references changed; retry operation");
  return report;
}
} // namespace syrnck
