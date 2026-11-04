#include "native_workspace.hpp"
#include "io_checkpoint.hpp"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <iterator>
#include <thread>
namespace syrnck {
namespace {
constexpr std::string_view active_name = "checkouts/active";
std::optional<ObjectKey> lookup(const Refs &refs, std::string_view name) {
  const auto found = refs.find(std::string(name));
  return found == refs.end() ? std::optional<ObjectKey>{} : std::optional{found->second};
}
bool inside(const fs::path &path, const fs::path &root) {
  auto p = path.begin();
  for (auto r = root.begin(); r != root.end(); ++r, ++p)
    if (p == path.end() || *p != *r)
      return false;
  return true;
}
bool present(const fs::path &path) {
  return fs::symlink_status(path).type() != fs::file_type::not_found;
}
void directory(const fs::path &path, bool create) {
  if (!present(path) && create) {
    require(fs::create_directory(path), "cannot create native checkout directory");
#ifndef _WIN32
    fs::permissions(path, fs::perms::owner_all);
#endif
    sync_directory(path.parent_path());
  }
  const auto status = fs::symlink_status(path);
  require(fs::is_directory(status) && !native_link_like(path, status), "invalid native checkout directory");
}
void remove_empty_directory(const fs::path &path, OperationOptions options) {
  std::error_code error;
  for (unsigned attempt = 0;; ++attempt) {
    throw_if_cancelled(options.stop);
    directory(path, false);
    const auto removed = fs::remove(path, error);
    if (!error) {
      require(removed, "cannot clean native checkout data");
      return;
    }
#ifdef _WIN32
    // Windows may briefly retain delete-pending files after successful removal.
    // Retry only this nonrecursive removal; never delete newly appeared entries.
    if (error == std::errc::directory_not_empty && attempt < 50) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }
#else
    (void)attempt;
#endif
    throw fs::filesystem_error("remove", path, error);
  }
}
struct Record {
  std::string id, nonce, branch, target, state = "preparing";
  fs::path workspace;
  std::optional<std::string> source;
  ObjectKey key{Kind::chunk, {}};
  Json identity() const {
    return Json{{"version", 1},     {"type", "native-checkout"},
                {"nonce", nonce},   {"directory", utf8(workspace)},
                {"branch", branch}, {"source", source ? Json(*source) : Json{}},
                {"target", target}};
  }
  Json json() const {
    auto result = identity();
    result["id"] = id;
    result["state"] = state;
    return result;
  }
  ObjectKey destination() const {
    return {Kind::revision, target};
  }
  std::optional<ObjectKey> origin() const {
    return source ? std::optional{ObjectKey{Kind::revision, *source}} : std::optional<ObjectKey>{};
  }
  std::string pin(std::string_view suffix) const {
    return "checkouts/pending/" + id + '/' + std::string(suffix);
  }
  std::string archive() const {
    return "archive/checkout/" + id;
  }
  std::string receipt() const {
    return "checkouts/receipts/" + id;
  }
  std::string retained() const {
    return "checkouts/completed/" + id;
  }
  fs::path data(const Store &store) const {
    return store.root() / "checkouts" / id;
  }
  ObjectKey save(const Store &store) const {
    return store.put(Kind::chunk, bytes(json().dump())).first;
  }
};
Record parse_record(const Store &store, const ObjectKey &key) {
  require(key.kind == Kind::chunk, "invalid native checkout record");
  const auto data = store.get(key);
  const auto value = parse_json(data);
  require(value.is_object() && value.size() == 9 && value.value("version", 0) == 1 &&
              value.value("type", "") == "native-checkout",
          "invalid native checkout record");
  Record record;
  record.key = key;
  record.id = value.at("id").get<std::string>();
  record.nonce = value.at("nonce").get<std::string>();
  record.branch = value.at("branch").get<std::string>();
  record.target = value.at("target").get<std::string>();
  record.state = value.at("state").get<std::string>();
  record.workspace = path_from_utf8(value.at("directory").get<std::string>());
  if (!value.at("source").is_null())
    record.source = value.at("source").get<std::string>();
  validate_id(record.id);
  validate_id(record.nonce);
  validate_id(record.target);
  if (record.source)
    validate_id(*record.source);
  validate_ref("heads/" + record.branch);
  validate_ref("staging/" + record.branch);
  require(record.state == "preparing" || record.state == "applying" || record.state == "aborting" ||
              record.state == "discarding" || record.state == "aborted" || record.state == "committed",
          "invalid native checkout record");
  require(record.workspace.is_absolute() && record.workspace == record.workspace.lexically_normal() &&
              record.workspace != store.root() && inside(store.root(), record.workspace) &&
              record.id == hash(bytes(record.identity().dump())) && record.json().dump() == text(data),
          "invalid native checkout record");
  return record;
}
std::optional<Record> active(const Store &store) {
  const auto refs = store.refs();
  const auto key = lookup(refs, active_name);
  if (!key)
    return {};
  auto record = parse_record(store, *key);
  require(lookup(refs, record.pin("target")) == record.destination() &&
              lookup(refs, record.pin("source")) == record.origin(),
          "native checkout retention changed");
  return record;
}
Json actions(const Store &store, const Record &record, OperationOptions options) {
  const auto difference = compare_native_snapshots(store, record.source, record.target, options);
  Json result = Json::array();
  for (const auto &row : difference["changes"]) {
    throw_if_cancelled(options.stop);
    if (row["content_changed"].get<bool>() || row["executable_changed"].get<bool>())
      result.push_back(row);
  }
  return result;
}
std::uint64_t touched_bytes(const Json &entries) {
  std::uint64_t total = 0;
  for (const auto &row : entries) {
    const auto amount =
        (row["target"].is_null() ? row["source"] : row["target"])["size"].get<std::uint64_t>();
    require(amount <= UINT64_MAX - total, "native checkout byte count overflow");
    total += amount;
  }
  return total;
}
// Resolve existing components without following links. Complete namespace
// inventories check aliases before application and before reference publication;
// avoid rescanning a whole directory for every individual file move.
fs::path working_path(const Store &store, const Record &record, const std::string &relative, bool create) {
  validate_path(relative);
  require(fs::canonical(record.workspace) == record.workspace, "native checkout workspace changed");
  auto path = record.workspace;
  const auto components = path_from_utf8(relative);
  for (auto it = components.begin(); it != components.end(); ++it) {
    const bool last = std::next(it) == components.end();
    if (present(path)) {
      const auto status = fs::symlink_status(path);
      require(fs::is_directory(status) && !native_link_like(path, status),
              "native checkout path became obstructed");
    }
    path /= *it;
    require(!inside(path, store.root()), "native checkout path became obstructed");
    if (!last) {
      if (!present(path) && create) {
        require(fs::create_directory(path), "cannot create native checkout directory");
        sync_directory(path.parent_path());
      }
      if (present(path)) {
        const auto status = fs::symlink_status(path);
        require(fs::is_directory(status) && !native_link_like(path, status),
                "native checkout path became obstructed");
      }
    } else if (present(path)) {
      const auto status = fs::symlink_status(path);
      require(fs::is_regular_file(status) && !native_link_like(path, status),
              "native checkout path became obstructed");
    }
  }
  return path;
}
class Transaction {
  const Store &store_;
  Bytes buffer_ = Bytes(1024 * 1024);

public:
  Record record;
  Json entries;
  Transaction(const Store &store, Record value, OperationOptions options)
      : store_(store), record(std::move(value)), entries(actions(store, record, options)) {}
  fs::path file(std::string_view area, std::size_t index) const {
    return record.data(store_) / area / (std::to_string(index) + ".bin");
  }
  void private_directories(bool create) const {
    const auto parent = store_.root() / "checkouts";
    if (!present(parent) && !create)
      return;
    directory(parent, create);
    const auto data = record.data(store_);
    if (!present(data) && !create)
      return;
    directory(data, create);
    for (const auto *name : {"new", "old"}) {
      if (present(data / name) || create)
        directory(data / name, create);
    }
  }
  bool matches(const fs::path &path, const Json &summary, const std::string &relative,
               OperationOptions options, bool check_mode = true) {
    if (!present(path))
      return false;
    const auto status = fs::symlink_status(path);
    require(fs::is_regular_file(status) && !native_link_like(path, status), "invalid native checkout file");
    bool executable = false;
#ifndef _WIN32
    executable =
        (status.permissions() & (fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec)) !=
        fs::perms::none;
    if (check_mode && executable != summary["executable"].get<bool>())
      return false;
#else
    (void)check_mode;
#endif
    const auto content = hash_native_file(
        {path, relative, fs::file_size(path), fs::last_write_time(path), executable}, buffer_, options);
    return content.size == summary["size"].get<std::uint64_t>() &&
           content.hash == summary["file_hash"].get<std::string>();
  }
  void state(std::string value, OperationOptions options) {
    auto next = record;
    next.state = std::move(value);
    const auto key = next.save(store_);
    require(store_.compare_update_refs({{std::string(active_name), record.key, key}}, {options.stop}),
            "native checkout references changed");
    next.key = key;
    record = std::move(next);
  }
  void expected_head() const {
    const auto refs = store_.refs();
    require(lookup(refs, "heads/" + record.branch) == record.origin() &&
                !refs.contains("staging/" + record.branch),
            "native checkout branch or staging changed");
  }
  void prepare(OperationOptions options);
  void apply(OperationOptions options);
  void undo(OperationOptions options);
  void cleanup(OperationOptions options);
  Json run(bool abort, OperationOptions options);
};
void Transaction::prepare(OperationOptions options) {
  private_directories(true);
  require(fs::is_empty(record.data(store_) / "old"), "invalid native checkout backup state");
  for (std::size_t i = 0; i < entries.size(); ++i) {
    throw_if_cancelled(options.stop);
    const auto &row = entries[i], &target = row["target"];
    if (target.is_null())
      continue;
    const auto path = row["path"].get<std::string>();
    const auto staged = file("new", i);
    if (!present(staged))
      store_.restore_file(target["manifest"].get<std::string>(), staged,
                          {options.stop, [&](const ProgressEvent &event) {
                             auto progress = event;
                             progress.path = path;
                             report_progress(options, progress);
                           }});
    else {
      const auto status = fs::symlink_status(staged);
      require(fs::is_regular_file(status) && !native_link_like(staged, status),
              "invalid native checkout file");
    }
    if (target["executable"].get<bool>())
      make_executable(staged);
    require(matches(staged, target, path, options), "native checkout prepared file changed");
  }
  expected_head();
  const auto plan = store_.plan_checkout(record.workspace, record.branch, record.target, false, options);
  require(plan["source"] == (record.source ? Json(*record.source) : Json{}),
          "native checkout references changed");
  for (const auto &conflict : plan["conflicts"])
    require(conflict["reason"] == "checkout_pending", "native checkout workspace is no longer clean");
  state("applying", options);
  SYRNCK_IO_CHECKPOINT("checkout.prepared", store_.root() / "checkout.state");
}
void Transaction::apply(OperationOptions options) {
  const auto total = touched_bytes(entries);
  std::uint64_t completed = 0;
  for (std::size_t i = 0; i < entries.size(); ++i) {
    const auto &row = entries[i], &source = row["source"], &target = row["target"];
    const auto path = row["path"].get<std::string>();
    report_progress(options, {ProgressPhase::exporting, path, completed, total});
    private_directories(false);
    const auto old = file("old", i), next = file("new", i);
    auto working = working_path(store_, record, path, !target.is_null());
    if (!source.is_null()) {
      if (!present(old)) {
        require(matches(working, source, path, options), "working file changed during native checkout");
        working = working_path(store_, record, path, false);
        move_exclusive(working, old);
      }
      require(matches(old, source, path, options), "native checkout backup changed; abort to restore it");
    } else
      require(!present(old), "invalid native checkout backup state");
    working = working_path(store_, record, path, !target.is_null());
    if (!target.is_null()) {
      if (present(next)) {
        require(matches(next, target, path, options), "native checkout prepared file changed");
        working = working_path(store_, record, path, true);
        require(!present(working), "native checkout destination appeared; preserve it before recovery");
        move_exclusive(next, working);
      } else
        require(matches(working, target, path, options), "working file changed during native checkout");
    } else
      require(!present(working), "native checkout destination appeared; preserve it before recovery");
    completed += (target.is_null() ? source : target)["size"].get<std::uint64_t>();
    report_progress(options, {ProgressPhase::exporting, path, completed, total, i + 1 == entries.size()});
  }
  SYRNCK_IO_CHECKPOINT("checkout.applied", store_.root() / "checkout.state");
  report_progress(options, {ProgressPhase::publishing, "checkout", completed, total});
  // This is the last observer. Recheck every desired file after it, then use
  // only stop tokens through the atomic branch/journal publication.
  const OperationOptions final_options{options.stop};
  std::vector<std::string> target_paths, removed_paths;
  for (const auto &[path, target] : read_native_snapshot(store_, record.target, final_options)) {
    require(matches(working_path(store_, record, path, false), target.json(), path, final_options),
            "working file changed during native checkout");
    target_paths.push_back(path);
  }
  for (const auto &row : entries)
    if (row["target"].is_null()) {
      require(!present(working_path(store_, record, row["path"].get<std::string>(), false)),
              "native checkout destination appeared; preserve it before recovery");
      removed_paths.push_back(row["path"].get<std::string>());
    }
  verify_native_checkout_namespace(store_, record.workspace, target_paths, removed_paths, final_options);
  expected_head();
  auto committed = record;
  committed.state = "committed";
  const auto key = committed.save(store_);
  std::vector<RefUpdate> updates{{"heads/" + record.branch, record.origin(), record.destination()},
                                 {std::string(active_name), record.key, key},
                                 {record.receipt(), {}, key},
                                 {record.retained(), {}, record.destination()}};
  if (record.source)
    updates.push_back({record.archive(), {}, record.origin()});
  require(store_.compare_update_refs(updates, final_options), "native checkout references changed");
  committed.key = key;
  record = std::move(committed);
  SYRNCK_IO_CHECKPOINT("checkout.committed", store_.root() / "checkout.state");
}
void Transaction::undo(OperationOptions options) {
  if (record.state == "discarding")
    return;
  const auto total = touched_bytes(entries);
  std::uint64_t completed = 0;
  for (std::size_t n = entries.size(); n != 0; --n) {
    const auto i = n - 1;
    const auto &row = entries[i], &source = row["source"], &target = row["target"];
    const auto path = row["path"].get<std::string>();
    report_progress(options, {ProgressPhase::exporting, path, completed, total});
    private_directories(false);
    const auto old = file("old", i), next = file("new", i);
    auto working = working_path(store_, record, path, false);
    if (present(old)) {
      require(!source.is_null(), "invalid native checkout backup state");
      const auto status = fs::symlink_status(old);
      require(fs::is_regular_file(status) && !native_link_like(old, status), "invalid native checkout file");
      if (!target.is_null() && !present(next) && present(working)) {
        require(matches(working, target, path, options),
                "working file changed; preserve it before aborting checkout");
        working = working_path(store_, record, path, false);
        move_exclusive(working, next);
        require(matches(next, target, path, options), "native checkout prepared file changed");
      }
      working = working_path(store_, record, path, true);
      require(!present(working), "native checkout destination appeared; preserve it before recovery");
      // A source edited just before its backup was taken is restored verbatim.
      move_exclusive(old, working);
    } else if (source.is_null()) {
      if (!present(next) && present(working)) {
        require(matches(working, target, path, options),
                "working file changed; preserve it before aborting checkout");
        working = working_path(store_, record, path, false);
        move_exclusive(working, next);
        require(matches(next, target, path, options), "native checkout prepared file changed");
      }
    } else if (!target.is_null())
      require(present(next), "invalid native checkout backup state");
    completed += (target.is_null() ? source : target)["size"].get<std::uint64_t>();
    report_progress(options, {ProgressPhase::exporting, path, completed, total, n == 1});
  }
}
void Transaction::cleanup(OperationOptions options) {
  private_directories(false);
  const auto data = record.data(store_);
  std::vector<fs::path> owned;
  if (present(data)) {
    for (const auto &entry : fs::directory_iterator(data))
      require(entry.path().filename() == "new" || entry.path().filename() == "old",
              "unexpected native checkout data");
    for (const auto *area : {"new", "old"}) {
      const auto parent = data / area;
      if (!present(parent))
        continue;
      for (const auto &entry : fs::directory_iterator(parent)) {
        throw_if_cancelled(options.stop);
        const auto name = utf8(entry.path().filename());
        const auto status = entry.symlink_status();
        require(fs::is_regular_file(status) && !native_link_like(entry.path(), status),
                "unexpected native checkout data");
        const bool temporary =
            std::string_view(area) == "new" && name.size() == 36 && name.starts_with(".tmp") &&
            std::all_of(name.begin() + 4, name.end(),
                        [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
        if (!temporary) {
          std::size_t index = 0;
          const auto end = name.size() >= 4 ? name.data() + name.size() - 4 : name.data();
          const auto parsed = std::from_chars(name.data(), end, index);
          require(parsed.ec == std::errc{} && parsed.ptr == end && index < entries.size() &&
                      std::to_string(index) + ".bin" == name,
                  "unexpected native checkout data");
          const auto &row = entries[index];
          const auto &summary = row[std::string_view(area) == "new" ? "target" : "source"];
          require(!summary.is_null() && (std::string_view(area) == "new" || record.state == "committed"),
                  "invalid native checkout backup state");
          require(matches(entry.path(), summary, row["path"].get<std::string>(), options,
                          record.state != "discarding"),
                  "native checkout private file changed; preserve it before cleanup");
        }
        owned.push_back(entry.path());
      }
    }
    for (const auto &path : owned) {
      throw_if_cancelled(options.stop);
      require(fs::remove(path), "cannot clean native checkout data");
    }
    for (const auto *area : {"new", "old"})
      if (present(data / area))
        remove_empty_directory(data / area, options);
    remove_empty_directory(data, options);
    sync_directory(data.parent_path());
  }
  SYRNCK_IO_CHECKPOINT("checkout.cleaned", store_.root() / "checkout.state");
  const auto refs = store_.refs();
  if (record.state == "committed") {
    require(lookup(refs, record.receipt()) == record.key &&
                lookup(refs, record.retained()) == record.destination() &&
                (!record.source || lookup(refs, record.archive()) == record.origin()),
            "native checkout retention changed");
  } else
    expected_head();
  require(store_.compare_update_refs({{std::string(active_name), record.key, {}},
                                      {record.pin("target"), record.destination(), {}},
                                      {record.pin("source"), record.origin(), {}}},
                                     {options.stop}),
          "native checkout references changed");
}
Json Transaction::run(bool abort, OperationOptions options) {
  throw_if_cancelled(options.stop);
  require(fs::canonical(record.workspace) == record.workspace, "native checkout workspace changed");
  if (abort) {
    require(record.state != "committed", "native checkout is committed; recovery can only finish cleanup");
    if (record.state == "preparing" || record.state == "applying")
      state(record.state == "preparing" ? "discarding" : "aborting", options);
  }
  if (record.state != "committed")
    expected_head();
  if (record.state == "applying" || record.state == "aborting") {
    std::vector<std::string> paths;
    for (const auto &entry : entries)
      paths.push_back(entry["path"].get<std::string>());
    verify_native_checkout_namespace(store_, record.workspace, paths, {}, options, false);
  }
  if (record.state == "preparing")
    prepare(options);
  if (record.state == "applying")
    apply(options);
  const bool committed = record.state == "committed";
  if (record.state == "aborting") {
    undo(options);
    state("aborted", options);
  }
  // Once branch publication succeeds, cleanup has no observers/cancellation.
  cleanup(committed ? OperationOptions{} : options);
  auto result = record.json();
  result["pending"] = false;
  result["state"] = committed ? "committed" : "aborted";
  result["archive"] = committed && record.source ? Json(record.archive()) : Json{};
  return result;
}
} // namespace
void require_no_native_checkout(const Store &store) {
  require(!store.refs().contains(std::string(active_name)),
          "native checkout is pending; run native checkout-recover");
}
Json Store::checkout_directory(const fs::path &workspace, std::string_view branch, std::string_view target,
                               OperationOptions options) const {
  throw_if_cancelled(options.stop);
  require_no_native_checkout(*this);
  const auto plan = plan_checkout(workspace, branch, target, false, options);
  require(plan["ready"].get<bool>(), "native checkout is blocked; inspect native checkout-plan");
  if (plan["source"] == Json(target))
    return Json{{"version", 1}, {"pending", false}, {"state", "unchanged"}, {"target", target}};
  Record record;
  record.workspace = fs::canonical(workspace);
  record.nonce = hex(random_bytes(32));
  record.branch = branch;
  record.target = target;
  if (!plan["source"].is_null())
    record.source = plan["source"].get<std::string>();
  record.id = hash(bytes(record.identity().dump()));
  if (present(root() / "checkouts"))
    directory(root() / "checkouts", false);
  require(!present(record.data(*this)), "native checkout transaction directory already exists");
  record.key = record.save(*this);
  std::vector<RefUpdate> updates{{std::string(active_name), {}, record.key},
                                 {"heads/" + record.branch, record.origin(), record.origin()},
                                 {"staging/" + record.branch, {}, {}},
                                 {record.pin("target"), {}, record.destination()}};
  if (record.source)
    updates.push_back({record.pin("source"), {}, record.origin()});
  require(compare_update_refs(updates, options), "native checkout references changed");
  return Transaction(*this, std::move(record), options).run(false, options);
}
Json Store::recover_checkout(bool abort, OperationOptions options) const {
  throw_if_cancelled(options.stop);
  auto record = active(*this);
  if (!record)
    return Json{{"version", 1}, {"pending", false}};
  return Transaction(*this, std::move(*record), options).run(abort, options);
}
Json Store::checkout_status(const std::optional<std::string> &transaction) const {
  auto record = active(*this);
  bool pending = record.has_value();
  if (transaction) {
    validate_id(*transaction);
    if (!record || record->id != *transaction) {
      const auto key = lookup(refs(), "checkouts/receipts/" + *transaction);
      require(key.has_value(), "unknown native checkout transaction");
      record = parse_record(*this, *key);
      require(record->id == *transaction && record->state == "committed", "invalid native checkout record");
      pending = false;
    }
  }
  if (!record)
    return Json{{"version", 1}, {"pending", false}};
  auto result = record->json();
  result["pending"] = pending;
  result["data_directory"] = utf8(record->data(*this));
  Json files = Json::array();
  const auto changes = actions(*this, *record, {});
  for (std::size_t i = 0; i < changes.size(); ++i)
    files.push_back(Json{{"path", changes[i]["path"]}, {"slot", std::to_string(i) + ".bin"}});
  result["files"] = std::move(files);
  return result;
}
} // namespace syrnck
