#include "cli_agent.hpp"
#include "cli_identity.hpp"
#include "native_context.hpp"
#include "watch_journal.hpp"
#include <algorithm>
#include <iostream>
#include <thread>
namespace syrnck::cli {
namespace {
using Clock = std::chrono::steady_clock;
using Milliseconds = std::chrono::milliseconds;
void output(const Json &frame) {
  std::cout << frame.dump(-1, ' ', false, Json::error_handler_t::replace) << '\n' << std::flush;
  require(std::cout.good(), "output write failed");
}
Json semantic(Json value) {
  value.erase("observed"); value.erase("committed"); value.erase("transfer");
  value.erase("full_scan");
  value.erase("expected_remote_revision"); return value;
}
}
void AgentCommand::validate() const {
  require(interval_ms >= 20 && interval_ms <= 60000, "watch interval must be in 20..60000 milliseconds");
  require(debounce_ms <= 5000, "watch debounce must be in 0..5000 milliseconds");
  require(audit_ms >= 1000 && audit_ms <= 3600000, "watch audit must be in 1000..3600000 milliseconds");
  require(remote_poll_ms >= 100 && remote_poll_ms <= 3600000, "agent remote poll must be in 100..3600000 milliseconds");
  require(duration_ms == 0 || (duration_ms >= 100 && duration_ms <= 86400000), "watch duration must be zero or in 100..86400000 milliseconds");
  require(max_updates <= 1000000, "watch update limit must be in 0..1000000");
  require(max_paths >= 1 && max_paths <= 65536, "watch path limit must be in 1..65536");
  validate_ref(remote_branch);
  require(remote_branch.find('/') == remote_branch.npos && remote_branch.size() <= 64,
      "native remote names must be single components of at most 64 bytes");
  if (branch) validate_native_target(*branch);
  require(!conflict_copies || bidirectional, "agent conflict copies require --bidirectional");
}
int run_agent(const fs::path &directory, const fs::path &storage, const AgentCommand &command,
              const net::Remote &remote, const std::function<std::unique_ptr<Store>()> &open_store,
              Language language, OperationOptions operation, net::TransferOptions transfer) {
  command.validate(); transfer.validate();
  const auto started = Clock::now();
  const auto deadline = command.duration_ms ? started + Milliseconds(command.duration_ms) : Clock::time_point::max();
  std::uint64_t sequence = 0, updates = 0;
  Json previous;
  std::unique_ptr<Store> initial;
  while (!initial && Clock::now() < deadline) {
    throw_if_cancelled(operation.stop);
    try { initial = open_store(); }
    catch (const Error &error) {
      if (std::string_view(error.what()) != "store busy: another process holds its lock") throw;
      Json frame{{"version", 1}, {"state", "unavailable"}, {"reason", "store_busy"},
          {"message", localize(error.what(), language)}, {"local_revision", Json{}}};
      if (previous.is_null()) {
        previous = semantic(frame);
        frame["sequence"] = ++sequence; frame["type"] = "sync"; frame["backend"] = Json{};
        output(frame); ++updates;
        if (command.max_updates && updates >= command.max_updates) return 1;
      }
      if (Clock::now() >= deadline) return 1;
      std::this_thread::sleep_for(Milliseconds(20));
    }
  }
  if (!initial) return 1;
  require(!command.branch || *command.branch == initial->native_target(), "agent branch must match the selected HEAD");
  load_native_identity(*initial);
  auto identity = initial->take_native_identity();
  const auto context = std::string(initial->native_workspace_name());
  const auto working_directory = command.directory_from_workspace
      ? path_from_utf8(initial->native_head()["directory"].get<std::string>()) : directory;
  net::NativeSync sync(*initial, working_directory,
      {command.remote_branch, !command.commits_only, command.bidirectional, command.bidirectional, command.conflict_copies});
  // Own only this agent's session lock, never the Store lock while waiting.
  auto ownership = lock_watch_journal(native_context_local_path(fs::canonical(storage), context, "agent-push.lock").parent_path(), "agent-push.lock");
  WorkspaceWatchOptions watch_options{command.polling, static_cast<std::size_t>(command.max_paths)};
  watch_options.external_store = !context.empty();
  WorkspaceWatcher watcher(working_directory, storage, watch_options, operation.stop);
  initial.reset();
  auto audit = started, poll = started, retry = started;
  WorkspaceChanges changes; changes.full_scan = true;
  bool pending = true, deferred = false;
  int exit_code = 0;
  while (Clock::now() < deadline) {
    throw_if_cancelled(operation.stop);
    const auto now = Clock::now();
    if (now >= audit) { changes.full_scan = true; pending = true; }
    if (now >= poll) pending = true;
    if (pending && (!deferred || now >= retry)) {
      Json frame;
      std::unique_ptr<Store> store;
      bool attached = false;
      const auto detach = [&] {
        if (attached) { identity = store->take_native_identity(); attached = false; }
      };
      try {
        store = open_store();
        if (identity) {
          store->use_native_identity(std::move(identity->identity), std::move(identity->signer), identity->require_signature);
          identity.reset(); attached = true;
        }
        try { frame = sync.tick(*store, remote, changes, operation, transfer); }
        catch (...) { detach(); throw; }
        detach();
        exit_code = frame["state"] == "blocked" ? 2 : 0;
      } catch (const net::PublicationUncertain &error) {
        frame = Json{{"version", 1}, {"state", "blocked"}, {"reason", "publication_uncertain"}, {"message", localize(error.what(), language)}};
        exit_code = 3;
      } catch (const Cancelled &) { throw; }
        catch (const Error &error) {
          frame = Json{{"version", 1}, {"state", "unavailable"}, {"reason", "operation_failed"}, {"message", localize(error.what(), language)}};
          exit_code = 1;
        } catch (const fs::filesystem_error &error) {
          frame = Json{{"version", 1}, {"state", "unavailable"}, {"reason", "filesystem_failed"}, {"message", error.what()}};
          exit_code = 1;
        }
      // A failed upload can follow a successful local snapshot. Report its
      // durable root as well, so offline progress is visible between retries.
      if (store && !frame.contains("local_revision")) {
        try { frame["local_revision"] = store->native_head()["revision"]; }
        catch (const Error &) { /* Preserve the original failure. */ }
      }
      store.reset();
      deferred = exit_code != 0;
      if (deferred) {
        sync.invalidate(); changes.full_scan = true;
        retry = Clock::now() + Milliseconds(1000); audit = retry;
      }
      if (frame["state"] == "blocked" && !frame.contains("message")) frame["message"] = localize("Native synchronization requires attention", language);
      const auto comparable = semantic(frame);
      if (previous.is_null() || comparable != previous) {
        previous = comparable;
        frame["sequence"] = ++sequence; frame["type"] = "sync"; frame["backend"] = watcher.backend();
        output(frame); ++updates;
        if (command.max_updates && updates >= command.max_updates) return exit_code;
      }
      if (!deferred) {
        if (frame.value("full_scan", false)) audit = Clock::now() + Milliseconds(command.audit_ms);
        changes = {};
      }
      poll = Clock::now() + Milliseconds(command.remote_poll_ms);
      pending = false;
    }
    // An expired remote-poll timer must not turn the one-second retry backoff
    // into a zero-timeout busy loop while the endpoint or Store is unavailable.
    const auto until = std::min(deadline, deferred ? retry : std::min(poll, audit));
    const auto left = std::chrono::duration_cast<Milliseconds>(until - Clock::now());
    const auto wait = Milliseconds(std::clamp<std::int64_t>(left.count(), 0, static_cast<std::int64_t>(command.interval_ms)));
    const auto batch = watcher.wait(wait, operation.stop);
    if (batch.changed()) {
      changes.merge(batch, static_cast<std::size_t>(command.max_paths)); pending = true;
      const auto coalesce = std::min(deadline, Clock::now() + Milliseconds(command.debounce_ms));
      while (Clock::now() < coalesce) {
        const auto remaining = std::chrono::duration_cast<Milliseconds>(coalesce - Clock::now());
        if (remaining.count() <= 0) break;
        changes.merge(watcher.wait(remaining, operation.stop), static_cast<std::size_t>(command.max_paths));
      }
    }
  }
  throw_if_cancelled(operation.stop);
  return exit_code;
}
} // namespace syrnck::cli
