#include "internal.hpp"
#include <algorithm>
namespace syrnck {
namespace {
struct Reader {
  std::string_view remaining;
  std::size_t work;
  OperationOptions options;
  void use(std::size_t amount) {
    throw_if_cancelled(options.stop);
    require(amount <= work, "text patch exceeds work budget");
    work -= amount;
  }
  std::string_view take() {
    const auto end = remaining.find('\n');
    require(end != remaining.npos, "truncated text patch");
    const auto line = remaining.substr(0, end + 1);
    use(line.size() + 1);
    remaining.remove_prefix(line.size());
    return line;
  }
};
std::string_view syntax(std::string_view line) {
  line.remove_suffix(1); // Reader requires a terminating LF.
  if (line.ends_with('\r'))
    line.remove_suffix(1);
  return line;
}
void index_header(std::string_view line) {
  require(line.starts_with("index "), "unsupported text patch header");
  line.remove_prefix(6);
  const auto space = line.find(' ');
  if (space != line.npos) {
    const auto mode = line.substr(space + 1);
    require(mode == "100644" || mode == "100755", "unsupported text patch header");
    line = line.substr(0, space);
  }
  const auto dots = line.find("..");
  require(dots != line.npos, "unsupported text patch header");
  for (const auto value : {line.substr(0, dots), line.substr(dots + 2)})
    require(value.size() >= 4 && value.size() <= 64 &&
                std::all_of(value.begin(), value.end(),
                            [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }),
            "unsupported text patch header");
}
std::size_t number(std::string_view &line) {
  require(!line.empty() && line[0] >= '0' && line[0] <= '9', "invalid text patch hunk header");
  std::size_t value = 0;
  while (!line.empty() && line[0] >= '0' && line[0] <= '9') {
    const auto digit = static_cast<std::size_t>(line[0] - '0');
    require(value < 10000 || (value == 10000 && digit == 0), "text patch range exceeds 100000 lines");
    value = value * 10 + digit;
    line.remove_prefix(1);
  }
  return value;
}
struct Range {
  std::size_t start, count;
};
Range range(std::string_view &line) {
  const auto start = number(line);
  std::size_t count = 1;
  if (line.starts_with(',')) {
    line.remove_prefix(1);
    count = number(line);
  }
  require(start || !count, "invalid text patch hunk header");
  return {start - (count != 0), count};
}
bool marker(std::string_view line) {
  return line.starts_with("\\ No newline at end of file\n") ||
         line.starts_with("\\ No newline at end of file\r\n");
}
} // namespace
TextPatchResult apply_text_patch(std::span<const std::uint8_t> source, std::span<const std::uint8_t> patch,
                                 std::string_view path, TextDiffOptions limits, OperationOptions options) {
  throw_if_cancelled(options.stop);
  limits.validate();
  validate_path(path);
  require(source.size() <= limits.max_file_bytes, "text patch source exceeds file budget");
  require(patch.size() <= limits.max_output_bytes, "text patch input exceeds byte budget");
  require(is_utf8_text(source, options) && is_utf8_text(patch, options), "text patch requires UTF-8 text");
  const std::string_view before(reinterpret_cast<const char *>(source.data()), source.size());
  Reader reader{{reinterpret_cast<const char *>(patch.data()), patch.size()}, limits.max_work, options};
  std::vector<std::string_view> lines;
  for (std::size_t pos = 0; pos < before.size();) {
    throw_if_cancelled(options.stop);
    require(lines.size() < limits.max_lines, "text patch exceeds line budget");
    const auto lf = before.find('\n', pos);
    const auto end = lf == before.npos ? before.size() : lf + 1;
    lines.push_back(before.substr(pos, end - pos));
    pos = end;
  }
  auto header = syntax(reader.take());
  if (header.starts_with("diff --git ")) {
    require(header == "diff --git a/" + std::string(path) + " b/" + std::string(path),
            "text patch path does not match selected file");
    header = syntax(reader.take());
    if (header.starts_with("index ")) {
      index_header(header);
      header = syntax(reader.take());
    }
  }
  require(header == "--- a/" + std::string(path) || header == "--- " + std::string(path),
          "text patch path does not match selected file");
  header = syntax(reader.take());
  require(header == "+++ b/" + std::string(path) || header == "+++ " + std::string(path),
          "text patch path does not match selected file");
  TextPatchResult result;
  std::size_t cursor = 0, target_lines = 0;
  auto append = [&](std::string_view line) {
    throw_if_cancelled(options.stop);
    require(result.content.empty() || result.content.back() == '\n', "text patch joins unterminated lines");
    require(line.size() <= limits.max_file_bytes - result.content.size(),
            "text patch result exceeds file budget");
    require(target_lines < limits.max_lines, "text patch exceeds line budget");
    result.content += line;
    ++target_lines;
  };
  auto copy = [&] {
    reader.use(lines[cursor].size() + 1);
    append(lines[cursor++]);
  };
  while (!reader.remaining.empty()) {
    require(result.hunks < 100000, "text patch exceeds hunk limit");
    header = syntax(reader.take());
    require(header.starts_with("@@ -"), "invalid text patch hunk header");
    header.remove_prefix(4);
    const auto old = range(header);
    require(header.starts_with(" +"), "invalid text patch hunk header");
    header.remove_prefix(2);
    const auto next = range(header);
    require(header.starts_with(" @@") && (header.size() == 3 || header[3] == ' '),
            "invalid text patch hunk header");
    require(old.start >= cursor && old.start <= lines.size() && old.count <= lines.size() - old.start,
            "text patch source range does not match");
    while (cursor < old.start)
      copy();
    require(target_lines == next.start, "text patch target range does not match");
    std::size_t consumed = 0, emitted = 0;
    while (consumed < old.count || emitted < next.count) {
      auto row = reader.take();
      const auto kind = row[0];
      row.remove_prefix(1);
      require(kind == ' ' || kind == '-' || kind == '+', "invalid text patch body");
      if (marker(reader.remaining)) {
        (void)reader.take();
        row.remove_suffix(1);
      }
      require(!row.empty(), "text patch contains an empty unterminated line");
      if (kind != '+') {
        require(consumed < old.count && cursor < lines.size(), "text patch hunk counts do not match");
        reader.use(row.size() + 1);
        require(lines[cursor] == row, "text patch context does not match source");
        ++cursor;
        ++consumed;
      }
      if (kind != '-') {
        require(emitted < next.count, "text patch hunk counts do not match");
        append(row);
        ++emitted;
      }
      result.additions += kind == '+';
      result.deletions += kind == '-';
    }
    ++result.hunks;
    report_progress(options, {ProgressPhase::planning, path, result.hunks});
  }
  require(result.hunks && (result.additions || result.deletions), "text patch contains no changes");
  while (cursor < lines.size())
    copy();
  require(is_utf8_text(bytes(result.content), options), "text patch requires UTF-8 text");
  report_progress(options, {ProgressPhase::planning, path, result.hunks, result.hunks, true});
  return result;
}
} // namespace syrnck
