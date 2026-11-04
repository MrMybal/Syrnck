#include "internal.hpp"
#include <syrnck/git_lfs.hpp>
#include <chrono>
#include <iostream>
#include <thread>

using namespace syrnck;
namespace {
std::string acknowledgement_mode, acknowledgement_ref, acknowledgement_source;
}
namespace syrnck::git::detail {
void test_transaction_acknowledgement() {
  if (acknowledgement_mode == "advanced_ack")
    require(process({"git", "update-ref", acknowledgement_ref, acknowledgement_source}).status == 0,
            "cannot simulate concurrent reference advancement");
  if (acknowledgement_mode == "lost_ack" || acknowledgement_mode == "advanced_ack")
    throw Error("simulated lost reference acknowledgement");
}
} // namespace syrnck::git::detail
int main(int argc, char **argv) {
  try {
    require(argc == 2, "expected migration probe mode");
    const std::string mode = argv[1], branch = "probe-" + mode, reference = "refs/heads/" + branch;
    auto run = [](std::vector<std::string> arguments) {
      arguments.insert(arguments.begin(), "git");
      auto result = process(arguments);
      require(result.status == 0, "probe Git command failed");
      while (!result.out.empty() && (result.out.back() == '\n' || result.out.back() == '\r'))
        result.out.pop_back();
      return result.out;
    };
    const auto source = run({"rev-parse", "HEAD"});
    acknowledgement_mode = mode;
    acknowledgement_ref = reference;
    acknowledgement_source = source;
    Store store(git::common_store("."));
    const auto before = store.refs();
    std::stop_source stop;
    std::jthread watcher;
    bool induced = false, cancelled = false, failed = false, uncertain = false;
    std::optional<Json> result;
    auto options = store.settings().ingest_options();
    options.stop = stop.get_token();
    options.progress = [&](const ProgressEvent &event) {
      if (mode == "cancel_ingest" && event.phase == ProgressPhase::ingesting && event.bytes > 0) {
        induced = true;
        stop.request_stop();
      }
      if (event.phase != ProgressPhase::publishing || event.path != reference)
        return;
      induced = true;
      if (mode == "cancel_publish")
        stop.request_stop();
      else if (mode == "race_ref")
        (void)run({"update-ref", reference, source});
      else if (mode == "race_symbolic")
        (void)run({"symbolic-ref", reference, "refs/heads/absent"});
      else if (mode == "late_stop") {
        auto path = store.root().parent_path() / reference;
        watcher = std::jthread([&, path](std::stop_token finish) {
          while (!finish.stop_requested()) {
            std::error_code error;
            if (fs::exists(path, error) && !error) {
              stop.request_stop();
              return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          }
        });
      }
    };
    try {
      result = git::migrate_lfs(store, ".", "HEAD", git::lfs_objects("."), branch, options);
    } catch (const Cancelled &) {
      cancelled = true;
    } catch (const git::LfsPublicationUncertain &) {
      failed = uncertain = true;
    } catch (const Error &) {
      failed = true;
    }
    if (watcher.joinable()) {
      watcher.request_stop();
      watcher.join();
    }
    require(induced, "migration probe never reached requested boundary");
    require(run({"rev-parse", "HEAD"}) == source, "migration changed HEAD");
    if (mode == "cancel_ingest" || mode == "cancel_publish") {
      require(cancelled && !result, "migration did not honor pre-publication cancellation");
      require(process({"git", "show-ref", "--verify", "--quiet", reference}).status == 1,
              "cancelled migration published branch");
      if (mode == "cancel_ingest")
        require(store.refs() == before, "cancelled ingest changed pins");
      else
        require(!store.refs().empty(), "prepared migration lost retained assets");
    } else if (mode == "race_ref" || mode == "race_symbolic") {
      require(failed && !result && !cancelled, "migration ignored reference conflict");
      if (mode == "race_ref")
        require(run({"rev-parse", reference}) == source, "foreign branch overwritten");
      else
        require(run({"symbolic-ref", reference}) == "refs/heads/absent", "foreign symbolic ref overwritten");
      require(!store.refs().empty(), "conflicted migration lost retained assets");
    } else if (mode == "lost_ack") {
      require(result && !failed && !cancelled && (*result)["reconciled"].get<bool>(),
              "lost acknowledgement was not reconciled");
      require(run({"rev-parse", reference}) == (*result)["commit"].get<std::string>(),
              "reconciled migration result differs from branch");
    } else if (mode == "advanced_ack") {
      require(uncertain && !result && !cancelled, "advanced publication was not reported as uncertain");
      require(run({"rev-parse", reference}) == source, "reconciliation overwrote subsequent reference");
      require(!store.refs().empty(), "uncertain migration lost retained assets");
    } else if (mode == "late_stop") {
      require(result && !failed && !cancelled && stop.stop_requested(), "late stop invalidated publication");
      require(run({"rev-parse", reference}) == (*result)["commit"].get<std::string>(),
              "migration publication result differs from branch");
    } else
      throw Error("unknown migration probe mode");
    require(store.fsck() > 0 || mode == "cancel_ingest", "migration Store verification failed");
    std::cout << "Migration boundary verified: " << mode << '\n';
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
