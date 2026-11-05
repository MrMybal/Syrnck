#include "internal.hpp"
#include <iostream>
#include <random>
using namespace syrnck;
namespace {
Json merge(std::string_view base, std::string_view current, std::string_view incoming,
           TextDiffOptions limits = {}, OperationOptions options = {}) {
  return merge_text(bytes(base), bytes(current), bytes(incoming), limits, options);
}
void clean(std::string_view base, std::string_view current, std::string_view incoming,
           std::string_view expected) {
  const auto result = merge(base, current, incoming);
  require(result["status"] == "clean" && result["content"].get<std::string>() == expected &&
              result["conflicts"].empty(),
          "unexpected clean text merge");
  const auto swapped = merge(base, incoming, current);
  require(swapped["status"] == "clean" && swapped["content"] == result["content"],
          "text merge is not symmetric");
}
void conflict(std::string_view base, std::string_view current, std::string_view incoming,
              std::size_t count = 1) {
  const auto result = merge(base, current, incoming);
  require(result["status"] == "conflict" && result["content"].is_null() &&
              result["conflicts"].size() == count,
          "unexpected text conflict");
  const auto swapped = merge(base, incoming, current);
  require(swapped["status"] == "conflict" && swapped["conflicts"].size() == count,
          "text conflict is not symmetric");
  for (std::size_t i = 0; i < count; ++i) {
    require(result["conflicts"][i]["base"] == swapped["conflicts"][i]["base"] &&
                result["conflicts"][i]["current"] == swapped["conflicts"][i]["incoming"] &&
                result["conflicts"][i]["incoming"] == swapped["conflicts"][i]["current"],
            "swapped text conflict coordinates differ");
  }
}
void limited(std::string_view base, std::string_view current, std::string_view incoming,
             TextDiffOptions limits, std::string_view reason) {
  const auto result = merge(base, current, incoming, limits);
  require(result["status"] == "limited" && result["reason"].get<std::string>() == reason &&
              result["content"].is_null() && result["conflicts"].empty() &&
              result["current_changes"].is_null(),
          "expected limit " + std::string(reason) + ": " + result.dump());
}
void examples() {
  clean("", "", "", "");
  clean("", "first\n", "", "first\n");
  clean("", "same", "same", "same");
  clean("a\nb\nc\n", "A\nb\nc\n", "a\nb\nC\n", "A\nb\nC\n");
  clean("a\nb\n", "A\nb\n", "a\nB\n", "A\nB\n");
  clean("a\nb\nc", "a\ninsert\nb\nc", "a\nb\nC", "a\ninsert\nb\nC");
  clean("a\r\nb\r\nc", "A\r\nb\r\nc", "a\r\nb\r\nC", "A\r\nb\r\nC");
  clean("a\nb\nc\n", "b\nc\n", "a\nb\n", "b\n");
  clean("x\ny\n", "", "", "");
  conflict("", "ours\n", "theirs\n");
  conflict("a\nb\n", "A\nb\n", "B\nb\n");
  conflict("a\nb\n", "b\n", "A\nb\n");
  conflict("a\nb\n", "insert\na\nb\n", "A\nb\n");
  conflict("a\nb\n", "a\ninsert\nb\n", "A\nb\n");
  conflict("a\nb\nc\nd\ne\n", "A\nb\nc\nd\nE\n", "X\nb\nc\nd\nY\n", 2);
  // One edit spans two edits from the other side: one connected conflict.
  conflict("a\nb\nc\nd\ne\n", "a\nB\nc\nD\ne\n", "a\ncombined\ne\n");
  const auto coordinates = merge("a\nb\nc\n", "prefix\na\nB\nc\n", "a\nother\nc\n");
  require(coordinates["conflicts"][0]["base"] == Json{{"start", 2}, {"count", 1}} &&
              coordinates["conflicts"][0]["current"] == Json{{"start", 3}, {"count", 1}} &&
              coordinates["conflicts"][0]["incoming"] == Json{{"start", 2}, {"count", 1}},
          "conflict line offsets lost earlier insertions");
  const auto insertion = merge("a\n", "a\nx\n", "a\ny\n");
  require(insertion["conflicts"][0]["base"] == Json{{"start", 1}, {"count", 0}},
          "empty base range has wrong preceding-line coordinate");
  for (const auto &value : {std::string("nul\0", 4), std::string("\xff"), std::string("a\rb")}) {
    const auto result = merge(value, value, value);
    require(result["status"] == "binary" && result["content"].is_null(),
            "binary equality bypassed classification");
  }
}
void budgets() {
  TextDiffOptions limits;
  limits.max_file_bytes = 2;
  limited("abc", "a", "a", limits, "file_limit");
  limited("a\n", "b\n", "a\nc", limits, "file_limit");
  limits.max_file_bytes = 5;
  limited("a\n", "x\na\n", "a\ny\n", limits, "result_file_limit");
  limits = {};
  limits.max_lines = 1;
  limited("a\nb\n", "", "", limits, "line_limit");
  limits.max_lines = 2;
  limited("a\n", "x\na\n", "a\ny\n", limits, "result_line_limit");
  limits = {};
  limits.max_work = 1;
  limited("a\n", "b\n", "c\n", limits, "work_limit");
  limits = {};
  limits.max_output_bytes = 1024;
  const std::string quotes(500, '"');
  limited("", quotes, "", limits, "output_limit");
  std::string base, ours, theirs;
  for (unsigned i = 0; i < 30; ++i) {
    base += "base" + std::to_string(i) + "\nkeep" + std::to_string(i) + "\n";
    ours += "ours" + std::to_string(i) + "\nkeep" + std::to_string(i) + "\n";
    theirs += "theirs" + std::to_string(i) + "\nkeep" + std::to_string(i) + "\n";
  }
  limited(base, ours, theirs, limits, "output_limit");
  bool failed = false;
  try {
    limits.max_lines = 0;
    merge("", "", "", limits);
  } catch (const Error &) {
    failed = true;
  }
  require(failed, "invalid merge limits accepted");
}
void random_cases() {
  std::mt19937 random(73551);
  for (unsigned trial = 0; trial < 1000; ++trial) {
    std::string base, ours, theirs, expected;
    // Distinct original lines and separated edit sites give an independent
    // expected composition without relying on the diff's edit script.
    for (unsigned i = 0; i < 24; ++i) {
      const auto line = "original " + std::to_string(i) + (i == 23 && trial % 2 ? "" : "\n");
      const auto replacement = "changed " + std::to_string(i) + (i == 23 && trial % 2 ? "" : "\n");
      const auto choice = random() % 3;
      base += line;
      ours += choice == 0 ? replacement : line;
      theirs += choice == 1 ? replacement : line;
      expected += choice < 2 ? replacement : line;
    }
    clean(base, ours, theirs, expected);
    clean(base, base, ours, ours);
    clean(base, ours, ours, ours);
  }
}
void interruption() {
  std::stop_source stop;
  bool cancelled = false;
  try {
    merge("a\n", "A\n", "B\n", {}, {stop.get_token(), [&](const ProgressEvent &) { stop.request_stop(); }});
  } catch (const Cancelled &) {
    cancelled = true;
  }
  require(cancelled, "merge ignored cancellation");
  bool observed = false;
  try {
    merge("", "", "", {}, {{}, [](const ProgressEvent &) { throw Error("merge observer sentinel"); }});
  } catch (const Error &e) {
    observed = std::string_view(e.what()) == "merge observer sentinel";
  }
  require(observed, "merge swallowed observer failure");
}
} // namespace
int main() {
  try {
    examples();
    budgets();
    random_cases();
    interruption();
    std::cout << "Bounded conservative three-way text merge, exact bytes, coordinates and independent "
                 "compositions\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
