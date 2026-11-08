#pragma once
#include "internal.hpp"
#include <syrnck/i18n.hpp>
#include <syrnck/native_sync.hpp>
namespace syrnck::cli {
struct AgentCommand {
  std::string remote_branch = "main";
  std::optional<std::string> branch;
  bool commits_only = false, polling = false, directory_from_workspace = false, bidirectional = false;
  bool conflict_copies = false;
  std::uint64_t interval_ms = 500, debounce_ms = 75, audit_ms = 300000, remote_poll_ms = 5000;
  std::uint64_t duration_ms = 0, max_updates = 0, max_paths = 4096;
  void validate() const;
};
int run_agent(const fs::path &, const fs::path &, const AgentCommand &, const net::Remote &,
              const std::function<std::unique_ptr<Store>()> &, Language, OperationOptions, net::TransferOptions);
} // namespace syrnck::cli
