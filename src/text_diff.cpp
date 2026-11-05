#include "internal.hpp"
#include <algorithm>
#include <utf8proc.h>
namespace syrnck {
void TextDiffOptions::validate() const {
  require(context <= 100, "text diff context must be in 0..100");
  require(max_file_bytes >= 1 && max_file_bytes <= 4 * 1024 * 1024,
          "text diff file budget must be in 1..4194304 bytes");
  require(max_lines >= 1 && max_lines <= 100000, "text diff line limit must be in 1..100000");
  require(max_work >= 1 && max_work <= 256 * 1024 * 1024, "text diff work budget must be in 1..268435456");
  require(max_output_bytes >= 1024 && max_output_bytes <= 8 * 1024 * 1024,
          "text diff output budget must be in 1024..8388608 bytes");
}
namespace {
struct Limited {
  const char *reason;
};
struct Work {
  std::size_t remaining;
  const OperationOptions &options;
  void use(std::size_t n = 1) {
    throw_if_cancelled(options.stop);
    if (n > remaining)
      throw Limited{"work_limit"};
    remaining -= n;
  }
  bool equal(std::string_view a, std::string_view b) {
    use(a.size() == b.size() ? a.size() + 1 : 1);
    return a == b;
  }
};
bool is_text(std::span<const std::uint8_t> input, OperationOptions options) {
  for (std::size_t pos = 0; pos < input.size();) {
    if (pos % 4096 == 0)
      throw_if_cancelled(options.stop);
    utf8proc_int32_t cp = 0;
    const auto n =
        utf8proc_iterate(input.data() + pos, static_cast<utf8proc_ssize_t>(input.size() - pos), &cp);
    if (n <= 0 || (utf8proc_category(cp) == UTF8PROC_CATEGORY_CC && cp != '\t' && cp != '\n' &&
                   !(cp == '\r' && pos + 1 < input.size() && input[pos + 1] == '\n')))
      return false;
    pos += static_cast<std::size_t>(n);
  }
  return true;
}
using Lines = std::vector<std::string_view>;
Lines split(std::span<const std::uint8_t> input, std::size_t limit, OperationOptions options) {
  Lines lines;
  const std::string_view value(reinterpret_cast<const char *>(input.data()), input.size());
  for (std::size_t pos = 0; pos < value.size();) {
    throw_if_cancelled(options.stop);
    if (lines.size() == limit)
      throw Limited{"line_limit"};
    const auto newline = value.find('\n', pos);
    const auto end = newline == value.npos ? value.size() : newline + 1;
    lines.push_back(value.substr(pos, end - pos));
    pos = end;
  }
  return lines;
}
struct Edit {
  char kind;
  std::string_view line;
};
std::vector<Edit> edits(const Lines &left, const Lines &right, Work &work) {
  std::size_t prefix = 0, suffix = 0;
  while (prefix < left.size() && prefix < right.size() && work.equal(left[prefix], right[prefix]))
    ++prefix;
  while (suffix < left.size() - prefix && suffix < right.size() - prefix &&
         work.equal(left[left.size() - suffix - 1], right[right.size() - suffix - 1]))
    ++suffix;
  const int n = static_cast<int>(left.size() - prefix - suffix);
  const int m = static_cast<int>(right.size() - prefix - suffix);
  std::vector<Edit> result;
  result.reserve(left.size() + right.size());
  for (std::size_t i = 0; i < prefix; ++i)
    result.push_back({' ', left[i]});
  std::vector<Edit> middle;
  if (n == 0 || m == 0) {
    for (int i = 0; i < n; ++i) {
      work.use();
      middle.push_back({'-', left[prefix + i]});
    }
    for (int i = 0; i < m; ++i) {
      work.use();
      middle.push_back({'+', right[prefix + i]});
    }
  } else {
    // Myers frontiers store only reachable diagonals of each parity. The
    // retained trace is capped at 1,048,576 ints (4 MiB on supported targets).
    std::vector<std::vector<int>> trace;
    std::size_t cells = 0;
    auto at = [&](int d, int k) { return trace[d][static_cast<std::size_t>((k + d) / 2)]; };
    int distance = 0;
    bool done = false;
    for (int d = 0; d <= n + m && !done; ++d) {
      if (static_cast<std::size_t>(d + 1) > 1048576 - cells)
        throw Limited{"trace_limit"};
      cells += static_cast<std::size_t>(d + 1);
      trace.emplace_back(static_cast<std::size_t>(d + 1));
      for (int k = -d; k <= d; k += 2) {
        work.use();
        int x = d == 0                                                       ? 0
                : k == -d || (k != d && at(d - 1, k - 1) < at(d - 1, k + 1)) ? at(d - 1, k + 1)
                                                                             : at(d - 1, k - 1) + 1;
        int y = x - k;
        while (x < n && y < m && work.equal(left[prefix + x], right[prefix + y])) {
          ++x;
          ++y;
        }
        trace[d][static_cast<std::size_t>((k + d) / 2)] = x;
        if (x >= n && y >= m) {
          distance = d;
          done = true;
          break;
        }
      }
      report_progress(work.options, {ProgressPhase::planning, {}, static_cast<std::uint64_t>(d)});
    }
    int x = n, y = m;
    for (int d = distance; d > 0; --d) {
      work.use();
      const int k = x - y;
      const int previous = k == -d || (k != d && at(d - 1, k - 1) < at(d - 1, k + 1)) ? k + 1 : k - 1;
      const int px = at(d - 1, previous), py = px - previous;
      while (x > px && y > py) {
        middle.push_back({' ', left[prefix + --x]});
        --y;
      }
      if (x == px)
        middle.push_back({'+', right[prefix + y - 1]});
      else
        middle.push_back({'-', left[prefix + x - 1]});
      x = px;
      y = py;
    }
    while (x > 0 && y > 0) {
      middle.push_back({' ', left[prefix + --x]});
      --y;
    }
    std::reverse(middle.begin(), middle.end());
  }
  result.insert(result.end(), middle.begin(), middle.end());
  for (std::size_t i = left.size() - suffix; i < left.size(); ++i)
    result.push_back({' ', left[i]});
  return result;
}
Json hunks(const std::vector<Edit> &edits, TextDiffOptions limits, OperationOptions options) {
  Json result = Json::array();
  std::size_t cursor = 0, source = 0, target = 0, budget = 1024;
  while (cursor < edits.size()) {
    throw_if_cancelled(options.stop);
    auto change = cursor;
    while (change < edits.size() && edits[change].kind == ' ')
      ++change;
    if (change == edits.size())
      break;
    const auto start = change - std::min(change - cursor, limits.context);
    auto last = change;
    for (auto scan = change + 1; scan < edits.size(); ++scan) {
      if (edits[scan].kind != ' ') {
        if (scan - last - 1 > 2 * limits.context)
          break;
        last = scan;
      }
    }
    const auto end = last + 1 + std::min(edits.size() - last - 1, limits.context);
    while (cursor < start) {
      source += edits[cursor].kind != '+';
      target += edits[cursor].kind != '-';
      ++cursor;
    }
    const auto source_before = source, target_before = target;
    Json lines = Json::array();
    if (budget + 256 > limits.max_output_bytes)
      throw Limited{"output_limit"};
    budget += 256;
    while (cursor < end) {
      const auto &edit = edits[cursor++];
      source += edit.kind != '+';
      target += edit.kind != '-';
      const bool newline = edit.line.ends_with('\n');
      const auto value = edit.line.substr(0, edit.line.size() - newline);
      Json line{{"kind", edit.kind == '+'   ? "add"
                         : edit.kind == '-' ? "remove"
                                            : "context"},
                {"text", value},
                {"newline", newline}};
      const auto size = line.dump().size() + 1;
      if (size > limits.max_output_bytes - budget)
        throw Limited{"output_limit"};
      budget += size;
      lines.push_back(std::move(line));
    }
    const auto source_count = source - source_before, target_count = target - target_before;
    result.push_back(Json{{"source_start", source_before + (source_count != 0)},
                          {"source_count", source_count},
                          {"target_start", target_before + (target_count != 0)},
                          {"target_count", target_count},
                          {"lines", std::move(lines)}});
  }
  return result;
}
} // namespace
Json compare_text(std::span<const std::uint8_t> source, std::span<const std::uint8_t> target,
                  TextDiffOptions limits, OperationOptions options) {
  throw_if_cancelled(options.stop);
  limits.validate();
  Json report{{"version", 1},
              {"status", "text"},
              {"reason", Json{}},
              {"equal", Json{}},
              {"source_bytes", source.size()},
              {"target_bytes", target.size()},
              {"source_lines", Json{}},
              {"target_lines", Json{}},
              {"additions", Json{}},
              {"deletions", Json{}},
              {"hunks", Json::array()}};
  try {
    if (source.size() > limits.max_file_bytes || target.size() > limits.max_file_bytes)
      throw Limited{"file_limit"};
    report_progress(options, {ProgressPhase::planning, {}, 0});
    report["equal"] = std::ranges::equal(source, target);
    if (!is_text(source, options) || !is_text(target, options)) {
      report["status"] = "binary";
      report["reason"] = "non_text_input";
    } else {
      const auto left = split(source, limits.max_lines, options);
      const auto right = split(target, limits.max_lines, options);
      report["source_lines"] = left.size();
      report["target_lines"] = right.size();
      Work work{limits.max_work, options};
      const auto script = edits(left, right, work);
      auto groups = hunks(script, limits, options);
      report["additions"] =
          std::count_if(script.begin(), script.end(), [](const Edit &e) { return e.kind == '+'; });
      report["deletions"] =
          std::count_if(script.begin(), script.end(), [](const Edit &e) { return e.kind == '-'; });
      report["hunks"] = std::move(groups);
    }
  } catch (const Limited &limit) {
    report["status"] = "limited";
    report["reason"] = limit.reason;
  }
  report_progress(options, {ProgressPhase::planning, {}, 1, 1, true});
  return report;
}
} // namespace syrnck
