#include "internal.hpp"
#include <syrnck/git.hpp>
#include <iostream>
using namespace syrnck;
int main(int argc, char **argv) {
  try {
    require(argc == 2, "expected retention probe mode");
    const std::string mode = argv[1];
    auto run = [](std::vector<std::string> arguments, std::string_view input = {}) {
      arguments.insert(arguments.begin(), "git");
      auto result = process(arguments, {}, input);
      require(result.status == 0, "retention probe Git command failed");
      while (!result.out.empty() && result.out.back() == '\n')
        result.out.pop_back();
      return result.out;
    };
    std::string commit;
    if (mode == "change_inventory") {
      auto tree = run({"mktree"});
      commit = run({"commit-tree", tree}, "concurrent empty history\n");
    }
    Store store(git::common_store("."));
    const auto before = store.refs();
    std::stop_source stop;
    bool induced = false, cancelled = false, failed = false;
    try {
      (void)git::reconcile_retention(
          store, ".", true, {stop.get_token(), [&](const ProgressEvent &event) {
                               if (mode == "cancel_verify" && event.phase == ProgressPhase::verifying) {
                                 induced = true;
                                 stop.request_stop();
                               } else if (mode == "cancel_publish" &&
                                          event.phase == ProgressPhase::publishing && event.path == "refs") {
                                 induced = true;
                                 stop.request_stop();
                               } else if (mode == "change_inventory" &&
                                          event.phase == ProgressPhase::planning && event.complete) {
                                 induced = true;
                                 (void)run({"update-ref", "refs/heads/concurrent-retention", commit});
                               }
                             }});
    } catch (const Cancelled &) {
      cancelled = true;
    } catch (const Error &) {
      failed = true;
    }
    require(induced, "retention probe boundary not reached");
    require(mode == "change_inventory" ? failed && !cancelled : cancelled && !failed,
            "retention probe failure classification differs");
    require(store.refs() == before, "failed retention changed references");
    require(store.fsck() > 0, "retention probe Store verification failed");
    std::cout << "Retention boundary verified: " << mode << '\n';
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
