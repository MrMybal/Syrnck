#pragma once
#include "fixture_cleanup.hpp"
#include <syrnck/core.hpp>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <functional>
#include <iostream>
#include <set>
#include <stdexcept>

namespace fixture_test {
struct CaseSelection {
  std::size_t index = 0, count = 1;
  bool list = false;
  static CaseSelection parse(int argc, char **argv) {
    CaseSelection result;
    bool selected = false;
    const auto number = [](std::string_view text) {
      std::size_t value = 0;
      const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
      if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        throw std::invalid_argument("invalid test shard number");
      return value;
    };
    for (int at = 1; at < argc; ++at) {
      const std::string_view argument = argv[at];
      if (argument == "--list" && !result.list) result.list = true;
      else if (argument == "--shard" && !selected && at + 1 < argc) {
        const std::string_view value = argv[++at];
        const auto slash = value.find('/');
        if (slash == value.npos) throw std::invalid_argument("test shard must be INDEX/COUNT");
        result.index = number(value.substr(0, slash));
        result.count = number(value.substr(slash + 1));
        selected = true;
      } else throw std::invalid_argument("unsupported test case argument");
    }
    if (result.index >= result.count || result.count > 64)
      throw std::invalid_argument("test shard must satisfy 0 <= INDEX < COUNT <= 64");
    return result;
  }
  std::pair<std::size_t, std::size_t> range(std::size_t size) const {
    if (size == 0 || count == 0 || count > size || count > 64 || index >= count)
      throw std::invalid_argument("empty or invalid test case partition");
    // floor(size * index / count), without overflowing size_t.
    const auto boundary = [&](std::size_t part) {
      return size / count * part + (size % count * part) / count;
    };
    return {boundary(index), boundary(index + 1)};
  }
};

// Private test execution only. Partition complete named scenarios, never their
// assertions. Delete each successful case after its handles have closed, with
// the same owned-child guard used by fixture setup. Flush stage timing before
// cleanup so a CI deadline identifies the work actually still running.
class TestCases {
public:
  explicit TestCases(CaseSelection selection = {}) : selection_(selection) {}
  void add(std::string name, std::function<void(const std::filesystem::path &)> function) {
    if (name.empty() || name.size() > 128 || !std::all_of(name.begin(), name.end(), [](unsigned char c) {
          return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        }) || !names_.insert(name).second || !function)
      throw std::invalid_argument("invalid or duplicate test case name");
    cases_.push_back({std::move(name), std::move(function)});
  }
  int run(const std::filesystem::path &owner) const {
    const auto [first, last] = selection_.range(cases_.size());
    if (selection_.list) {
      auto names = syrnck::Json::array();
      for (auto at = first; at < last; ++at) names.push_back(cases_[at].name);
      std::cout << syrnck::Json{{"version",1},{"total",cases_.size()},
          {"index",selection_.index},{"count",selection_.count},{"cases",names}}.dump() << std::endl;
      return 0;
    }
    const auto started = Clock::now();
    for (auto at = first; at < last; ++at) {
      const auto &entry = cases_[at];
      const auto before = Clock::now();
      report(entry.name, "running", before);
      try {
        entry.function(owner / entry.name);
        report(entry.name, "verified", before);
        const auto cleanup = Clock::now();
        report(entry.name, "cleaning", cleanup);
        if (std::filesystem::exists(owner / entry.name) || std::filesystem::is_symlink(owner / entry.name))
          remove_owned_tree(owner / entry.name, owner);
        report(entry.name, "cleaned", cleanup);
      } catch (...) {
        report(entry.name, "failed", before);
        throw;
      }
    }
    std::cout << syrnck::Json{{"version",1},{"verified",last-first},{"total",cases_.size()},
        {"index",selection_.index},{"count",selection_.count},
        {"elapsed_ms",milliseconds(started)}}.dump() << std::endl;
    return 0;
  }
private:
  using Clock = std::chrono::steady_clock;
  struct Entry { std::string name; std::function<void(const std::filesystem::path &)> function; };
  CaseSelection selection_;
  std::vector<Entry> cases_;
  std::set<std::string> names_;
  static std::int64_t milliseconds(Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
  }
  static void report(const std::string &name, const char *state, Clock::time_point start) {
    std::cout << syrnck::Json{{"case",name},{"state",state},{"elapsed_ms",milliseconds(start)}}.dump() << std::endl;
  }
};
}
