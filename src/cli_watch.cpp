#include "cli_watch.hpp"
#include "watch_journal.hpp"
#include <syrnck/watcher.hpp>
#include <syrnck/working_index.hpp>
#include <algorithm>
#include <iostream>
namespace syrnck::cli {
namespace {
using Clock = std::chrono::steady_clock;
using Milliseconds = std::chrono::milliseconds;
Json semantic_status(Json value) {
  if (value.contains("unstaged")) {
    value["unstaged"].erase("working_index");
    value["unstaged"].erase("hashed_bytes");
    value["unstaged"].erase("observation");
  } else {
    value.erase("working_index");
    value.erase("hashed_bytes");
    value.erase("observation");
  }
  return value;
}
void output(const Json &value) {
  // Filesystem diagnostics can contain bytes outside UTF-8 on POSIX. Their
  // display text must not corrupt the NDJSON stream or abort error recovery.
  std::cout << value.dump(-1, ' ', false, Json::error_handler_t::replace) << '\n' << std::flush;
  require(std::cout.good(), "output write failed");
}
void merge(WorkspaceChanges &into, const WorkspaceChanges &other, std::size_t limit) {
  into.full_scan |= other.full_scan;
  into.overflow |= other.overflow;
  into.polling |= other.polling;
  if (into.overflow) { into.paths.clear(); return; }
  std::set<std::string> paths(into.paths.begin(), into.paths.end());
  std::size_t budget = 0;
  for (const auto &path : paths) budget += path.size() + 64;
  for (const auto &path : other.paths) {
    if (paths.contains(path)) continue;
    if (paths.size() == limit || path.size() + 64 > 1024 * 1024 - budget) {
      into.paths.clear(); into.full_scan = into.overflow = true; return;
    }
    budget += path.size() + 64;
    paths.insert(path);
  }
  into.paths.assign(paths.begin(), paths.end());
}
struct Journal {
  std::string workspace, session = hash(random_bytes(16)), selection, last_digest, state;
  std::uint64_t generation = 0;
  Json previous, expected;
  bool loaded = false, durable = false;
  std::unique_ptr<FileLock> lock;
  Journal(const fs::path &root, const fs::path &storage, const NativeWatchCommand &command) {
    if (!command.journal) return;
    workspace = hash(bytes(utf8(root)));
    selection = hash(bytes(Json::array({command.branch ? Json(*command.branch) : Json{},
                                       command.revision ? Json(*command.revision) : Json{}, command.combined}).dump()));
    lock = lock_watch_journal(fs::canonical(storage));
  }
  void load(const Store &store, OperationOptions options) {
    const auto old = store.workspace_watch_journal(options);
    if (loaded) {
      require(old == expected, "workspace watch journal changed; retry operation");
      return;
    }
    previous = old.is_null() ? Json{{"state", "missing"}, {"session", Json{}}}
                            : Json{{"state", old["state"]}, {"session", old["session"]}};
    loaded = true;
    expected = old;
  }
  void save(const Store &store, std::string_view next_state, std::string_view backend,
            const WorkspaceChanges &batch, const Json &observation, OperationOptions options) {
    require(generation != UINT64_MAX, "workspace watch journal generation overflow");
    require(store.workspace_watch_journal(options) == expected, "workspace watch journal changed; retry operation");
    const auto document = Json{{"version", 1}, {"type", "native-watch-journal"}, {"workspace", workspace},
                               {"session", session}, {"generation", generation + 1}, {"selection", selection},
                               {"state", next_state}, {"backend", backend}, {"previous", previous},
                               {"batch", Json{{"full_scan", batch.full_scan}, {"overflow", batch.overflow},
                                              {"polling", batch.polling}, {"paths", batch.paths}}},
                               {"observation", observation}};
    durable = false;
    store.write_workspace_watch_journal(document, options);
    ++generation;
    expected = document;
    state = next_state;
    durable = true;
  }
  void annotate(Json &frame) const {
    if (lock) frame["journal"] = Json{{"session", session}, {"generation", generation},
                                      {"durable", durable}, {"previous", previous}};
  }
};
} // namespace
int watch_native(const fs::path &directory, const fs::path &storage, const NativeWatchCommand &command,
                 const std::function<std::unique_ptr<Store>()> &open_store, Language language,
                 OperationOptions operation) {
  require(command.interval_ms >= 20 && command.interval_ms <= 60000,
          "watch interval must be in 20..60000 milliseconds");
  require(command.debounce_ms <= 5000, "watch debounce must be in 0..5000 milliseconds");
  require(command.audit_ms >= 1000 && command.audit_ms <= 3600000,
          "watch audit must be in 1000..3600000 milliseconds");
  require(command.duration_ms == 0 || (command.duration_ms >= 100 && command.duration_ms <= 86400000),
          "watch duration must be zero or in 100..86400000 milliseconds");
  require(command.max_updates <= 1000000, "watch update limit must be in 0..1000000");
  require(command.max_paths >= 1 && command.max_paths <= 65536, "watch path limit must be in 1..65536");
  require(!(command.branch && command.revision), "choose either --branch or --revision");
  require(!(command.combined && command.revision), "combined native status cannot select an explicit revision");
  if (command.branch) validate_ref("heads/" + *command.branch);
  WorkspaceWatcher watcher(directory, storage, {command.polling, static_cast<std::size_t>(command.max_paths)}, operation.stop);
  const auto root = fs::canonical(directory);
  WorkspaceObservation observation(root, storage);
  Journal journal(root, storage, command);
  const auto started = Clock::now();
  const auto deadline = command.duration_ms ? started + Milliseconds(command.duration_ms) : Clock::time_point::max();
  auto audit = started, retry = started;
  std::uint64_t sequence = 0, updates = 0;
  Json previous;
  std::string last_error;
  WorkspaceChanges changes;
  changes.full_scan = true;
  bool pending = true, recovering = false, scheduled_audit = false;
  while (Clock::now() < deadline) {
    throw_if_cancelled(operation.stop);
    const auto now = Clock::now();
    if (now >= audit) { pending = true; changes.full_scan = true; scheduled_audit = true; }
    if (recovering && now >= retry) { pending = true; changes.full_scan = true; }
    if (pending && (!recovering || now >= retry)) {
      Json status;
      std::string error;
      std::unique_ptr<Store> store;
      journal.durable = false;
      try {
        // This is the only Store lifetime in the loop. Native notifications,
        // coalescing, waiting and writing stream output hold no Store lock.
        store = open_store();
        if (command.journal) {
          journal.load(*store, operation);
          // Quiet polling needs neither pending checkpoints nor repeated fsyncs.
          // Its changed result is still persisted before being emitted.
          if (changes.full_scan || !changes.paths.empty() || recovering)
            journal.save(*store, "pending", watcher.backend(), changes, Json{}, operation);
        }
        const auto head = store->native_head();
        require(head["directory"].is_null() || head["directory"].get<std::string>() == utf8(root),
                "native HEAD belongs to another workspace");
        require_no_native_checkout(*store);
        const auto branch = command.branch ? *command.branch : head["branch"].get<std::string>();
        if (command.combined) status = observation.workspace_status(*store, branch, changes, operation);
        else {
          std::optional<std::string> revision;
          if (command.revision) revision = store->resolve_native_revision(*command.revision, operation);
          else {
            const auto refs = store->refs();
            if (const auto found = refs.find("heads/" + branch); found != refs.end()) {
              require(found->second.kind == Kind::revision, "branch must point to a revision");
              revision = found->second.id;
            }
          }
          status = observation.status(*store, revision, changes, operation);
        }
        const auto &working = command.combined ? status["unstaged"] : status;
        changes.full_scan |= working["observation"]["full_scan"].get<bool>();
        if (command.journal) {
          const auto digest = hash(bytes(semantic_status(status).dump()));
          if (journal.state != "observed" || journal.last_digest != digest) {
            journal.save(*store, "observed", watcher.backend(), changes,
                Json{{"digest", digest}, {"base", working["base"]}, {"clean", status["clean"]},
                     {"workspace_files", working["workspace_files"]}}, operation);
            journal.last_digest = digest;
          }
          journal.durable = true;
        }
      } catch (const Cancelled &) { throw; }
        catch (const Error &failure) { error = failure.what(); }
        catch (const fs::filesystem_error &failure) { error = failure.what(); }
        catch (const Json::exception &failure) { error = failure.what(); }
      if (!error.empty()) {
        observation.invalidate();
        if (command.journal && store && journal.loaded) {
          try { journal.save(*store, "unavailable", watcher.backend(), changes, Json{}, operation); }
          catch (const Cancelled &) { throw; }
          catch (const Error &) { journal.durable = false; }
          catch (const fs::filesystem_error &) { journal.durable = false; }
        }
        store.reset();
        if (error != last_error) {
          auto frame = Json{{"version", 1}, {"sequence", ++sequence}, {"type", "error"},
                      {"code", "scan_failed"}, {"backend", watcher.backend()},
                      {"message", localize(error, language)}, {"status", Json{}}};
          journal.annotate(frame);
          output(frame);
        }
        last_error = std::move(error);
        recovering = true;
        retry = Clock::now() + Milliseconds(1000);
        audit = retry;
      } else {
        store.reset();
        const auto semantic = semantic_status(status);
        if (previous.is_null() || recovering || changes.full_scan || semantic != previous) {
          const auto reason = previous.is_null() ? "initial" : recovering ? "recovered" : changes.overflow ? "overflow"
                              : changes.full_scan ? (scheduled_audit ? "audit" : "rescan")
                              : changes.polling ? "polling" : "filesystem";
          auto frame = Json{{"version", 1}, {"sequence", ++sequence}, {"type", "status"}, {"reason", reason},
                      {"backend", watcher.backend()}, {"full_scan", changes.full_scan},
                      {"overflow", changes.overflow}, {"paths", changes.paths}, {"status", std::move(status)}};
          journal.annotate(frame);
          output(frame);
          ++updates;
        }
        previous = semantic;
        last_error.clear();
        recovering = false;
        if (changes.full_scan) audit = Clock::now() + Milliseconds(command.audit_ms);
        if (command.max_updates && updates >= command.max_updates) return 0;
      }
      changes = {};
      scheduled_audit = false;
      pending = false;
    }
    auto until = std::min(deadline, audit);
    if (recovering) until = std::min(until, retry);
    const auto left = std::chrono::duration_cast<Milliseconds>(until - Clock::now());
    const auto wait = Milliseconds(std::clamp<std::int64_t>(left.count(), 0,
                                  static_cast<std::int64_t>(command.interval_ms)));
    const auto batch = watcher.wait(wait, operation.stop);
    if (batch.changed()) {
      merge(changes, batch, static_cast<std::size_t>(command.max_paths));
      pending = true;
      const auto coalesce = std::min(deadline, Clock::now() + Milliseconds(command.debounce_ms));
      while (Clock::now() < coalesce) {
        const auto remaining = std::chrono::duration_cast<Milliseconds>(coalesce - Clock::now());
        if (remaining.count() <= 0) break;
        merge(changes, watcher.wait(remaining, operation.stop), static_cast<std::size_t>(command.max_paths));
      }
    }
  }
  throw_if_cancelled(operation.stop);
  return recovering ? 1 : 0;
}
} // namespace syrnck::cli
