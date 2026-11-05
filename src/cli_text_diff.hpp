#pragma once
#include "internal.hpp"
namespace syrnck::cli {
// Only consumes reports produced by Store::diff_revision_file.
inline std::string unified_text(const Json &report) {
  const auto &diff = report["diff"];
  require(diff["status"] == "text",
          "native text diff cannot render binary or limited input; use JSON output");
  if (diff["hunks"].empty())
    return {};
  const auto path = report["path"].get<std::string>();
  std::string output = "--- " + (report["source"].is_null() ? std::string("/dev/null") : "a/" + path) +
                       "\n+++ " + (report["target"].is_null() ? std::string("/dev/null") : "b/" + path) +
                       '\n';
  for (const auto &hunk : diff["hunks"]) {
    output += "@@ -" + hunk["source_start"].dump() + ',' + hunk["source_count"].dump() + " +" +
              hunk["target_start"].dump() + ',' + hunk["target_count"].dump() + " @@\n";
    for (const auto &line : hunk["lines"]) {
      output += line["kind"] == "add" ? '+' : line["kind"] == "remove" ? '-' : ' ';
      output += line["text"].get<std::string>();
      output += '\n';
      if (!line["newline"].get<bool>())
        output += "\\ No newline at end of file\n";
    }
  }
  return output;
}
} // namespace syrnck::cli
