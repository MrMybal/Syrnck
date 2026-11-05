#include "native_workspace.hpp"
#include <sstream>
namespace syrnck {
namespace {
struct Side {
  Json entry;
  std::uint64_t size = 0;
  std::string manifest;
  std::string content;
};
Side side(const Store &store, const std::optional<std::string> &revision, std::string_view path,
          OperationOptions options) {
  throw_if_cancelled(options.stop);
  Side result;
  if (!revision)
    return result;
  const auto snapshot = store.metadata(Kind::revision, *revision);
  const auto file = snapshot["files"].find(std::string(path));
  if (file == snapshot["files"].end())
    return result;
  result.manifest = (*file)["manifest"].get<std::string>();
  const auto manifest = store.metadata(Kind::manifest, result.manifest);
  result.size = manifest["size"].get<std::uint64_t>();
  result.entry = Json{{"manifest", result.manifest},
                      {"size", result.size},
                      {"file_hash", manifest["file_hash"]},
                      {"executable", (*file)["executable"]}};
  return result;
}
void load(const Store &store, Side &value, OperationOptions options) {
  if (value.entry.is_null())
    return;
  std::ostringstream output(std::ios::binary);
  store.reconstruct(value.manifest, output, options);
  value.content = std::move(output).str();
}
} // namespace
Json Store::diff_revision_file(const std::optional<std::string> &source,
                               const std::optional<std::string> &target, std::string_view path,
                               TextDiffOptions limits, OperationOptions options) const {
  throw_if_cancelled(options.stop);
  limits.validate();
  validate_path(path);
  auto before = side(*this, source, path, options), after = side(*this, target, path, options);
  require(!before.entry.is_null() || !after.entry.is_null(),
          "native text diff path is absent from both snapshots");
  const bool content_changed = before.entry.is_null() || after.entry.is_null() || before.size != after.size ||
                               before.entry["file_hash"] != after.entry["file_hash"];
  const bool mode_changed = !before.entry.is_null() && !after.entry.is_null() &&
                            before.entry["executable"] != after.entry["executable"];
  Json difference;
  const bool oversized = before.size > limits.max_file_bytes || after.size > limits.max_file_bytes;
  if (oversized) {
    difference = Json{{"version", 1},           {"status", "limited"},         {"reason", "file_limit"},
                      {"equal", Json{}},        {"source_bytes", before.size}, {"target_bytes", after.size},
                      {"source_lines", Json{}}, {"target_lines", Json{}},      {"additions", Json{}},
                      {"deletions", Json{}},    {"hunks", Json::array()}};
  } else {
    load(*this, before, options);
    load(*this, after, options);
    difference = compare_text(bytes(before.content), bytes(after.content), limits, options);
  }
  Json report{{"version", 1},
              {"path", path},
              {"source_revision", source ? Json(*source) : Json{}},
              {"target_revision", target ? Json(*target) : Json{}},
              {"source", before.entry},
              {"target", after.entry},
              {"equal", !content_changed && !mode_changed},
              {"content_changed", content_changed},
              {"executable_changed", mode_changed},
              {"manifest_changed", before.manifest != after.manifest},
              {"objects_verified", !oversized},
              {"diff", std::move(difference)}};
  require(report.dump().size() <= max_object_size, "native text diff report exceeds 16 MiB");
  report_progress(options, {ProgressPhase::planning, path, 1, 1, true});
  return report;
}
} // namespace syrnck
