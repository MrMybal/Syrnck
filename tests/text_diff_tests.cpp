#include "internal.hpp"
#include <algorithm>
#include <iostream>
#include <random>
using namespace syrnck;
namespace {
std::vector<std::string> lines(std::string_view text) {
  std::vector<std::string> result;
  while (!text.empty()) {
    const auto n = text.find('\n');
    const auto size = n == text.npos ? text.size() : n + 1;
    result.emplace_back(text.substr(0, size));
    text.remove_prefix(size);
  }
  return result;
}
std::size_t distance(const std::vector<std::string> &a, const std::vector<std::string> &b) {
  // Independent quadratic LCS oracle, only for tiny random inputs.
  std::vector<std::size_t> previous(b.size() + 1), next(previous);
  for (const auto &line : a) {
    for (std::size_t j = 0; j < b.size(); ++j)
      next[j + 1] = line == b[j] ? previous[j] + 1 : std::max(previous[j + 1], next[j]);
    previous.swap(next);
  }
  return a.size() + b.size() - 2 * previous.back();
}
void verify(const std::string &source, const std::string &target, std::size_t context) {
  TextDiffOptions options;
  options.context = context;
  const auto result = compare_text(bytes(source), bytes(target), options);
  require(result["status"] == "text" && result["equal"] == (source == target), "text classification differs");
  const auto before = lines(source), after = lines(target);
  require(result["source_lines"] == before.size() && result["target_lines"] == after.size(),
          "line counts differ");
  std::size_t cursor = 0, additions = 0, deletions = 0;
  std::string rebuilt;
  for (const auto &hunk : result["hunks"]) {
    const auto count = hunk["source_count"].get<std::size_t>();
    const auto start = hunk["source_start"].get<std::size_t>() - (count != 0);
    require(start >= cursor && start <= before.size(), "source hunk position differs");
    while (cursor < start)
      rebuilt += before[cursor++];
    const auto target_start = hunk["target_start"].get<std::size_t>() - (hunk["target_count"] != 0);
    require(lines(rebuilt).size() == target_start, "target hunk position differs");
    std::size_t consumed = 0, emitted = 0;
    for (const auto &row : hunk["lines"]) {
      auto line = row["text"].get<std::string>();
      if (row["newline"].get<bool>())
        line += '\n';
      if (row["kind"] != "add") {
        require(cursor < before.size() && before[cursor++] == line, "diff does not match source");
        ++consumed;
      }
      if (row["kind"] != "remove") {
        rebuilt += line;
        ++emitted;
      }
      additions += row["kind"] == "add";
      deletions += row["kind"] == "remove";
    }
    require(consumed == count && emitted == hunk["target_count"], "hunk counts differ");
  }
  while (cursor < before.size())
    rebuilt += before[cursor++];
  require(rebuilt == target, "hunks do not reconstruct target bytes");
  require(result["additions"] == additions && result["deletions"] == deletions &&
              additions + deletions == distance(before, after),
          "edit script is not minimal");
  require(result == compare_text(bytes(source), bytes(target), options), "text diff is not deterministic");
}
template <class F> void fails(F fn, std::string_view expected) {
  try {
    fn();
  } catch (const Error &e) {
    require(std::string_view(e.what()) == expected, "unexpected text diff error");
    return;
  }
  throw Error("expected text diff failure");
}
} // namespace
int main() {
  try {
    for (const auto &a : {std::string{}, std::string("a\nb\n"), std::string("a\nb"), std::string("\n\n"),
                          std::string("é\r\n漢字\r\n")})
      for (const auto &b : {std::string{}, std::string("a\nb\n"), std::string("a\nb"), std::string("\n\n"),
                            std::string("é\r\n漢字\r\n")})
        for (std::size_t context : {0, 1, 3, 100})
          verify(a, b, context);
    std::mt19937 random(0x5359524e);
    for (int run = 0; run < 2000; ++run) {
      auto make = [&] {
        std::string result;
        const auto count = random() % 15;
        for (unsigned i = 0; i < count; ++i) {
          result += static_cast<char>('a' + random() % 4);
          if (i + 1 < count || random() % 2)
            result += '\n';
        }
        return result;
      };
      verify(make(), make(), random() % 5);
    }
    for (const auto &binary : {std::string("a\0b", 3), std::string("\xff", 1), std::string("\xc0\xaf", 2),
                               std::string("\x1b[31m"), std::string("a\rb"), std::string("\xc2\x85", 2)}) {
      const auto report = compare_text(bytes(binary), bytes("text"));
      require(report["status"] == "binary" && report["hunks"].empty(), "binary input emitted text");
    }
    TextDiffOptions limits;
    limits.max_file_bytes = 1;
    require(compare_text(bytes("large"), bytes("x"), limits)["reason"] == "file_limit", "file limit ignored");
    limits = {};
    limits.max_lines = 1;
    require(compare_text(bytes("a\nb\n"), bytes("x"), limits)["reason"] == "line_limit",
            "line limit ignored");
    limits = {};
    limits.max_work = 1;
    require(compare_text(bytes("a\nb"), bytes("b\na"), limits)["reason"] == "work_limit",
            "work limit ignored");
    limits = {};
    limits.max_output_bytes = 1024;
    const auto limited = compare_text(bytes(std::string(2048, 'a')), bytes(std::string(2048, 'b')), limits);
    require(limited["reason"] == "output_limit" && limited["hunks"].empty() && limited.dump().size() < 1024,
            "output limit returned a partial diff");
    limits = {};
    limits.max_work = 256 * 1024 * 1024;
    std::string left, right;
    for (int i = 0; i < 2000; ++i) {
      left += "a\n";
      right += "b\n";
    }
    require(compare_text(bytes(left), bytes(right), limits)["reason"] == "trace_limit",
            "trace limit ignored");
    std::string large_source, large_target;
    for (int i = 0; i < 50000; ++i) {
      const auto line = std::to_string(i) + '\n';
      large_source += line;
      large_target += i == 1000 || i == 48000 ? "changed " + line : line;
    }
    const auto sparse = compare_text(bytes(large_source), bytes(large_target));
    require(sparse["status"] == "text" && sparse["hunks"].size() == 2 && sparse["additions"] == 2 &&
                sparse["deletions"] == 2 && sparse.dump().size() < 4096,
            "sparse edits in a large source exceeded default budgets");
    limits.context = 101;
    fails([&] { compare_text({}, {}, limits); }, "text diff context must be in 0..100");
    std::stop_source stop;
    stop.request_stop();
    fails([&] { compare_text({}, {}, {}, {stop.get_token()}); }, "operation cancelled");
    std::stop_source late;
    fails(
        [&] {
          compare_text(bytes(left), bytes(right), {},
                       {late.get_token(), [&](const ProgressEvent &) { late.request_stop(); }});
        },
        "operation cancelled");
    fails(
        [&] {
          compare_text({}, {}, {}, {{}, [](const ProgressEvent &) { throw Error("observer sentinel"); }});
        },
        "observer sentinel");
    std::cout << "Verified bounded text diff, UTF-8, exact newline bytes, LCS oracle and cancellation\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
