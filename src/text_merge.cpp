#include "text_edits.hpp"
#include <algorithm>
namespace syrnck {
namespace {
using namespace text_detail;
struct Change {
  std::size_t begin = 0, end = 0;
  Lines replacement;
};
std::vector<Change> changes(const Lines &base, const Lines &side, Work &work) {
  const auto script = edits(base, side, work);
  std::vector<Change> result;
  std::size_t pos = 0;
  bool active = false;
  for (const auto &row : script) {
    work.use();
    if (row.kind == ' ') {
      active = false;
      ++pos;
      continue;
    }
    if (!active) {
      result.push_back({pos, pos, {}});
      active = true;
    }
    if (row.kind == '-')
      result.back().end = ++pos;
    else
      result.back().replacement.push_back(row.line);
  }
  return result;
}
// Adjacent replacements are independent. Insertions at a replacement boundary
// are deliberately conservative: their order may depend on the author's intent.
bool overlaps(const Change &change, std::size_t begin, std::size_t end) {
  if (change.begin == change.end)
    return change.begin >= begin && change.begin <= end;
  if (begin == end)
    return begin >= change.begin && begin <= change.end;
  return change.begin < end && begin < change.end;
}
Lines variant(const Lines &base, std::size_t begin, std::size_t end, const std::vector<Change> &side,
              std::size_t first, std::size_t last, Work &work) {
  Lines result;
  auto append = [&](const Lines &lines, std::size_t a, std::size_t b) {
    for (auto i = a; i < b; ++i) {
      work.use();
      result.push_back(lines[i]);
    }
  };
  auto pos = begin;
  for (auto i = first; i < last; ++i) {
    append(base, pos, side[i].begin);
    append(side[i].replacement, 0, side[i].replacement.size());
    pos = side[i].end;
  }
  append(base, pos, end);
  return result;
}
Json range(std::size_t before, std::size_t count) {
  return Json{{"start", before + (count != 0)}, {"count", count}};
}
} // namespace
Json merge_text(std::span<const std::uint8_t> base, std::span<const std::uint8_t> current,
                std::span<const std::uint8_t> incoming, TextDiffOptions limits, OperationOptions options) {
  using namespace text_detail;
  throw_if_cancelled(options.stop);
  limits.validate();
  Json report{{"version", 1},
              {"status", "clean"},
              {"reason", Json{}},
              {"base_bytes", base.size()},
              {"current_bytes", current.size()},
              {"incoming_bytes", incoming.size()},
              {"base_lines", Json{}},
              {"current_lines", Json{}},
              {"incoming_lines", Json{}},
              {"current_changes", Json{}},
              {"incoming_changes", Json{}},
              {"conflicts", Json::array()},
              {"content", Json{}}};
  try {
    if (base.size() > limits.max_file_bytes || current.size() > limits.max_file_bytes ||
        incoming.size() > limits.max_file_bytes)
      throw Limited{"file_limit"};
    report_progress(options, {ProgressPhase::planning, {}, 0});
    if (!is_utf8_text(base, options) || !is_utf8_text(current, options) || !is_utf8_text(incoming, options)) {
      report["status"] = "binary";
      report["reason"] = "non_text_input";
    } else {
      const auto original = split(base, limits.max_lines, options);
      const auto ours = split(current, limits.max_lines, options);
      const auto theirs = split(incoming, limits.max_lines, options);
      report["base_lines"] = original.size();
      report["current_lines"] = ours.size();
      report["incoming_lines"] = theirs.size();
      Work work{limits.max_work, options};
      const auto left = changes(original, ours, work), right = changes(original, theirs, work);
      report["current_changes"] = left.size();
      report["incoming_changes"] = right.size();
      std::size_t a = 0, b = 0, pos = 0, current_pos = 0, incoming_pos = 0, line_count = 0;
      std::size_t conflict_budget = report.dump().size();
      std::string content;
      auto append = [&](std::string_view line) {
        work.use(line.size() + 1);
        if (!report["conflicts"].empty())
          return;
        if (line.size() > limits.max_file_bytes - content.size())
          throw Limited{"result_file_limit"};
        if (line_count == limits.max_lines)
          throw Limited{"result_line_limit"};
        if (!content.empty() && !content.ends_with('\n'))
          throw Limited{"unterminated_line_join"};
        content += line;
        ++line_count;
      };
      while (a < left.size() || b < right.size()) {
        work.use();
        const bool first_left = b == right.size() || (a < left.size() && left[a].begin <= right[b].begin);
        const auto &first = first_left ? left[a] : right[b];
        const auto begin = first.begin;
        auto end = first.end;
        for (auto i = pos; i < begin; ++i)
          append(original[i]);
        current_pos += begin - pos;
        incoming_pos += begin - pos;
        const auto a0 = a, b0 = b;
        if (first_left)
          ++a;
        else
          ++b;
        bool grew;
        do {
          grew = false;
          while (a < left.size() && overlaps(left[a], begin, end)) {
            work.use();
            end = std::max(end, left[a++].end);
            grew = true;
          }
          while (b < right.size() && overlaps(right[b], begin, end)) {
            work.use();
            end = std::max(end, right[b++].end);
            grew = true;
          }
        } while (grew);
        const auto our_part = variant(original, begin, end, left, a0, a, work);
        const auto their_part = variant(original, begin, end, right, b0, b, work);
        const bool both = a != a0 && b != b0;
        bool same = both && our_part.size() == their_part.size();
        for (std::size_t i = 0; same && i < our_part.size(); ++i)
          same = work.equal(our_part[i], their_part[i]);
        if (both && !same) {
          Json conflict{{"base", range(begin, end - begin)},
                        {"current", range(current_pos, our_part.size())},
                        {"incoming", range(incoming_pos, their_part.size())}};
          const auto size = conflict.dump().size() + 1;
          if (size > limits.max_output_bytes - conflict_budget)
            throw Limited{"output_limit"};
          conflict_budget += size;
          report["conflicts"].push_back(std::move(conflict));
          content.clear();
        } else {
          for (const auto line : a != a0 ? our_part : their_part)
            append(line);
        }
        current_pos += our_part.size();
        incoming_pos += their_part.size();
        pos = end;
      }
      for (auto i = pos; i < original.size(); ++i)
        append(original[i]);
      if (report["conflicts"].empty())
        report["content"] = std::move(content);
      else {
        report["status"] = "conflict";
        report["reason"] = "overlapping_changes";
      }
    }
    if (report.dump().size() > limits.max_output_bytes)
      throw Limited{"output_limit"};
  } catch (const Limited &limit) {
    report["status"] = "limited";
    report["reason"] = limit.reason;
    report["content"] = Json{};
    report["current_changes"] = report["incoming_changes"] = Json{};
    report["conflicts"] = Json::array();
  }
  report_progress(options, {ProgressPhase::planning, {}, 1, 1, true});
  return report;
}
} // namespace syrnck
