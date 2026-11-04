#include "git_internal.hpp"
#include <syrnck/git.hpp>
#include <algorithm>
#include <sstream>
namespace syrnck::git {
namespace {
std::string remote_key(std::string_view name) {
  require(!name.empty() && name.size() <= 128 &&
              std::all_of(name.begin(), name.end(),
                          [](char c) {
                            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                   (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
                          }),
          "invalid Git remote name");
  return "remote." + std::string(name) + ".syrnckUrl";
}
bool zero(std::string_view oid) {
  return std::all_of(oid.begin(), oid.end(), [](char c) { return c == '0'; });
}
} // namespace
void configure_remote(const fs::path &directory, std::string_view name, std::string_view url) {
  require(!url.empty() && url.size() <= 4096 && url.find_first_of("\r\n\0", 0, 3) == url.npos,
          "invalid block server URL");
  auto result = detail::local_git(directory, {"config", "--local", remote_key(name), std::string(url)});
  require(result.status == 0, "cannot configure Git block remote");
}
std::string remote_url(const fs::path &directory, std::string_view name) {
  auto result = detail::local_git(directory, {"config", "--local", "--get", remote_key(name)}, {}, 8192);
  require(result.status == 0 && !result.out.empty(), "configure a Syrnck block URL for this Git remote");
  return detail::trim_line(std::move(result.out));
}
namespace {
std::string encode_checkout(const CheckoutPolicy &policy) {
  if (policy.mode == CheckoutMode::download) {
    (void)remote_key(policy.remote);
    return "download:" + policy.remote;
  }
  require(policy.remote.empty(), "checkout remote is only available in download mode");
  require(policy.mode == CheckoutMode::local || policy.mode == CheckoutMode::skip,
          "invalid Git checkout mode");
  return policy.mode == CheckoutMode::skip ? "skip" : "local";
}
} // namespace
void configure_checkout(const fs::path &directory, const CheckoutPolicy &policy) {
  auto value = encode_checkout(policy);
  if (policy.mode == CheckoutMode::download)
    (void)remote_url(directory, policy.remote);
  auto result = detail::local_git(directory, {"config", "--local", "syrnck.checkout", value});
  require(result.status == 0, "cannot configure Git checkout policy");
}
CheckoutPolicy checkout_policy(const fs::path &directory) {
  auto result = detail::local_git(directory, {"config", "--local", "--get", "syrnck.checkout"}, {}, 1024);
  if (result.status == 1)
    return {};
  require(result.status == 0, "cannot read Git checkout policy");
  auto value = detail::trim_line(std::move(result.out));
  CheckoutPolicy policy;
  if (value == "skip")
    policy.mode = CheckoutMode::skip;
  else if (value.starts_with("download:")) {
    policy.mode = CheckoutMode::download;
    policy.remote = value.substr(9);
  } else
    require(value == "local", "invalid Git checkout mode");
  (void)encode_checkout(policy);
  return policy;
}
void install_pre_push(const fs::path &directory, const fs::path &executable, const fs::path &store) {
  auto configured = detail::local_git(directory, {"config", "--get", "core.hooksPath"}, {}, 8192);
  require(configured.status == 1, "custom core.hooksPath requires manual Syrnck hook integration");
  auto hooks = common_store(directory).parent_path() / "hooks";
  require(!fs::is_symlink(fs::symlink_status(hooks)), "Git hooks directory cannot be a symlink");
  fs::create_directories(hooks);
  require(fs::is_directory(hooks), "invalid Git hooks directory");
  auto target = hooks / "pre-push";
  std::error_code error;
  auto status = fs::symlink_status(target, error);
  require(!error || status.type() == fs::file_type::not_found, "cannot inspect Git hook");
  auto content = "#!/bin/sh\n# Syrnck pre-push v1\nexec " + detail::shell_quote(fs::absolute(executable)) +
                 " --store " + detail::shell_quote(fs::absolute(store)) + " git pre-push \"$@\" 1>&2\n";
  if (status.type() != fs::file_type::not_found) {
    require(fs::is_regular_file(status) && text(read_bytes(target, 16384)) == content,
            "existing pre-push hook differs; refusing to overwrite it");
  } else {
    atomic_write(target, bytes(content), false);
  }
  make_executable(target);
  sync_directory(hooks);
}
std::vector<std::string> push_commits(const fs::path &directory, std::istream &input,
                                      OperationOptions operation) {
  std::set<std::string> tips;
  std::string line;
  std::size_t total = 0, lines = 0;
  auto parse = [&] {
    require(++lines <= 4096, "pre-push reference limit exceeded");
    std::istringstream fields(line);
    std::string local_ref, local_oid, remote_ref, remote_oid, extra;
    require(bool(fields >> local_ref >> local_oid >> remote_ref >> remote_oid) && !(fields >> extra),
            "invalid pre-push input");
    detail::validate_oid(local_oid);
    detail::validate_oid(remote_oid);
    require(local_oid.size() == remote_oid.size(), "inconsistent pre-push object format");
    if (!zero(local_oid))
      tips.insert(detail::resolve_commit(directory, local_oid, operation.stop));
    line.clear();
  };
  char c = 0;
  while (input.get(c)) {
    throw_if_cancelled(operation.stop);
    require(++total <= 1024 * 1024 && line.size() < 4096 && c != '\0', "pre-push input exceeds size limit");
    if (c == '\n')
      parse();
    else
      line += c;
  }
  require(!input.bad() && input.eof() && line.empty(), "invalid pre-push input");
  if (tips.empty())
    return {};
  auto shallow =
      detail::local_git(directory, {"rev-parse", "--is-shallow-repository"}, {}, 1024, operation.stop);
  require(shallow.status == 0 && detail::trim_line(shallow.out) == "false",
          "pre-push requires complete Git history");
  auto grafts = common_store(directory).parent_path() / "info/grafts";
  require(!fs::is_symlink(fs::symlink_status(grafts)) &&
              (!fs::exists(grafts) || (fs::is_regular_file(grafts) && fs::file_size(grafts) == 0)),
          "pre-push does not support Git graft history");
  std::string roots;
  for (const auto &tip : tips)
    roots += tip + '\n';
  auto commits =
      detail::local_git(directory, {"rev-list", "--topo-order", "--reverse", "--max-count=100001", "--stdin"},
                        roots, 8 * 1024 * 1024, operation.stop);
  require(commits.status == 0, "cannot enumerate pushed Git history");
  std::vector<std::string> result;
  std::istringstream output(commits.out);
  while (std::getline(output, line)) {
    detail::validate_oid(line);
    require(result.size() < 100000, "pre-push commit limit exceeded");
    result.push_back(line);
  }
  require(output.eof(), "invalid Git history response");
  return result;
}
} // namespace syrnck::git
