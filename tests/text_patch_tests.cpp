#include "internal.hpp"
#include <iostream>
#include <random>
#include <source_location>
using namespace syrnck;
namespace {
template <class F>
void fails(F fn, std::string_view expected = {},
           std::source_location where = std::source_location::current()) {
  try {
    fn();
  } catch (const Error &e) {
    require(expected.empty() || std::string_view(e.what()) == expected,
            "unexpected patch failure at " + std::to_string(where.line()) + ": " + e.what());
    return;
  }
  throw Error("expected patch failure at " + std::to_string(where.line()));
}
std::string encode(const Json &diff) {
  std::string result = "--- a/code.cpp\n+++ b/code.cpp\n";
  for (const auto &hunk : diff["hunks"]) {
    result += "@@ -" + std::to_string(hunk["source_start"].get<std::size_t>()) + "," +
              std::to_string(hunk["source_count"].get<std::size_t>()) + " +" +
              std::to_string(hunk["target_start"].get<std::size_t>()) + "," +
              std::to_string(hunk["target_count"].get<std::size_t>()) + " @@\n";
    for (const auto &row : hunk["lines"]) {
      result += row["kind"] == "add" ? '+' : row["kind"] == "remove" ? '-' : ' ';
      result += row["text"].get<std::string>();
      result += '\n';
      if (!row["newline"].get<bool>())
        result += "\\ No newline at end of file\n";
    }
  }
  return result;
}
void roundtrip(const std::string &source, const std::string &target, std::size_t context) {
  TextDiffOptions options;
  options.context = context;
  const auto diff = compare_text(bytes(source), bytes(target), options);
  require(diff["status"] == "text", "patch test generated non-text data");
  const auto patch = encode(diff);
  if (source == target) {
    fails([&] { apply_text_patch(bytes(source), bytes(patch), "code.cpp", options); });
    return;
  }
  const auto result = apply_text_patch(bytes(source), bytes(patch), "code.cpp", options);
  require(result.content == target && result.hunks == diff["hunks"].size() &&
              result.additions == diff["additions"] && result.deletions == diff["deletions"],
          "patch roundtrip changed bytes or counts");
}
void malformed() {
  const std::string prefix = "--- a/code.cpp\n+++ b/code.cpp\n";
  const std::string patch = prefix + "@@ -1 +1 @@\n-old\n+new\n";
  require(apply_text_patch(bytes("old\n"), bytes(patch), "code.cpp").content == "new\n",
          "omitted counts rejected");
  const auto git = "diff --git a/code.cpp b/code.cpp\nindex 1234567..abcdef0 100644\n" + patch;
  require(apply_text_patch(bytes("old\n"), bytes(git), "code.cpp").content == "new\n",
          "Git preamble rejected");
  require(apply_text_patch(bytes("old\n"), bytes(prefix + "@@ -1 +1 @@ section\n-old\n+new\n"), "code.cpp")
                  .content == "new\n",
          "section heading rejected");
  for (std::size_t n = 0; n < patch.size(); ++n)
    fails([&] { apply_text_patch(bytes("old\n"), bytes(std::string_view(patch).substr(0, n)), "code.cpp"); });
  for (const auto &invalid : std::vector<std::string>{
           "--- a/other.cpp\n+++ b/code.cpp\n@@ -1 +1 @@\n-old\n+new\n",
           "--- a/code.cpp\n+++ /dev/null\n@@ -1 +0,0 @@\n-old\n", prefix + "@@ -0 +1 @@\n-old\n+new\n",
           prefix + "@@ -1,100001 +1 @@\n-old\n+new\n",
           prefix + "@@ -9999999999999999999999 +1 @@\n-old\n+new\n", prefix + "@@ -2 +1 @@\n-old\n+new\n",
           prefix + "@@ -1 +2 @@\n-old\n+new\n", prefix + "@@ -1,2 +1 @@\n-old\n+new\n",
           prefix + "@@ -1 +1,2 @@\n-old\n+new\n", prefix + "@@ -1 +1 @@\n-wrong\n+new\n",
           prefix + "@@ -1 +1 @@\n?old\n+new\n",
           prefix + "@@ -1 +1 @@\n-old\n+new\n\\ No newline at end of file\nextra\n",
           prefix + "@@ -1 +1 @@\n-old\n+\n\\ No newline at end of file\n", patch + patch,
           patch + "@@ -1 +1 @@\n-old\n+new\n",
           "diff --git a/code.cpp b/code.cpp\nold mode 100644\nnew mode 100755\n" + patch,
           "diff --git a/code.cpp b/code.cpp\nindex not-a-hash\n" + patch})
    fails([&] { apply_text_patch(bytes("old\n"), bytes(invalid), "code.cpp"); });
  fails([&] { apply_text_patch(bytes("old\n"), bytes(patch), "../code.cpp"); });
  fails(
      [&] {
        apply_text_patch(bytes("old\n"), bytes(patch), "code.cpp", {},
                         {{}, [](const ProgressEvent &) { throw Error("patch observer sentinel"); }});
      },
      "patch observer sentinel");
  std::stop_source stop;
  stop.request_stop();
  fails([&] { apply_text_patch(bytes("old\n"), bytes(patch), "code.cpp", {}, {stop.get_token()}); },
        "operation cancelled");
  // An insertion after an unterminated retained line must not concatenate bytes.
  fails([&] { apply_text_patch(bytes("old"), bytes(prefix + "@@ -1,0 +2 @@\n+new\n"), "code.cpp"); },
        "text patch joins unterminated lines");
}
void budgets() {
  const std::string prefix = "--- a/code.cpp\n+++ b/code.cpp\n";
  auto patch = prefix + "@@ -1 +1 @@\n-a\n+bigger\n";
  TextDiffOptions limit;
  limit.max_file_bytes = 3;
  fails([&] { apply_text_patch(bytes("a\n"), bytes(patch), "code.cpp", limit); },
        "text patch result exceeds file budget");
  fails([&] { apply_text_patch(bytes("large\n"), bytes(patch), "code.cpp", limit); },
        "text patch source exceeds file budget");
  limit = {};
  limit.max_lines = 1;
  fails([&] { apply_text_patch(bytes("a\nb\n"), bytes(patch), "code.cpp", limit); },
        "text patch exceeds line budget");
  fails(
      [&] {
        apply_text_patch(bytes("a\n"), bytes(prefix + "@@ -1 +1,2 @@\n-a\n+x\n+y\n"), "code.cpp", limit);
      },
      "text patch exceeds line budget");
  limit = {};
  limit.max_work = 1;
  fails([&] { apply_text_patch(bytes("a\n"), bytes(patch), "code.cpp", limit); },
        "text patch exceeds work budget");
  limit = {};
  limit.max_output_bytes = 1024;
  fails(
      [&] {
        apply_text_patch(bytes("a\n"), bytes(prefix + "@@ -1 +1 @@ " + std::string(1500, 'x') + "\n-a\n+b\n"),
                         "code.cpp", limit);
      },
      "text patch input exceeds byte budget");
  for (const auto &binary :
       std::vector<std::string>{std::string("a\0\n", 3), std::string("\xff\n", 2), "lone\r"})
    fails([&] { apply_text_patch(bytes(binary), bytes(patch), "code.cpp"); },
          "text patch requires UTF-8 text");
}
} // namespace
int main() {
  try {
    for (const auto &a : std::vector<std::string>{"", "a\nb\n", "a\nb", "\n\n", "utf8 \xc3\xa9\r\nlast"})
      for (const auto &b :
           std::vector<std::string>{"", "new\r\nend", "a\nb\n", "\n\n", "utf8 \xe2\x82\xac\n"})
        for (std::size_t context : {0, 1, 3, 100})
          roundtrip(a, b, context);
    std::mt19937 random(23817);
    auto sample = [&] {
      std::string value;
      for (unsigned n = random() % 18; n; --n)
        value += std::string(1, static_cast<char>('a' + random() % 5)) + (random() % 3 ? "\n" : "\r\n");
      if (!value.empty() && random() % 2) {
        value.pop_back();
        if (value.ends_with('\r'))
          value.pop_back();
      }
      return value;
    };
    for (unsigned n = 0; n < 1000; ++n) {
      const auto a = sample(), b = sample();
      roundtrip(a, b, random() % 5);
    }
    malformed();
    budgets();
    std::cout
        << "Verified exact unified patch roundtrips, strict contexts/ranges/paths, newlines and budgets\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
