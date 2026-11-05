#include "native_text_diff.hpp"
#include <fstream>
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
Json file_limit(std::uint64_t source, std::uint64_t target) {
  return Json{{"version", 1},           {"status", "limited"},    {"reason", "file_limit"},
              {"equal", Json{}},        {"source_bytes", source}, {"target_bytes", target},
              {"source_lines", Json{}}, {"target_lines", Json{}}, {"additions", Json{}},
              {"deletions", Json{}},    {"hunks", Json::array()}};
}
bool same_file(const std::optional<NativeFile> &a, const std::optional<NativeFile> &b) {
  if (!a || !b)
    return !a && !b;
  return a->path == b->path && a->size == b->size && a->modified == b->modified &&
         a->executable == b->executable;
}
std::string working_bytes(const NativeFile &file, OperationOptions options) {
  std::ifstream input(file.path, std::ios::binary);
  require(input.is_open(), "cannot open source file");
  std::string result;
  result.reserve(static_cast<std::size_t>(file.size));
  std::array<char, 65536> buffer{};
  report_progress(options, {ProgressPhase::verifying, file.relative, 0, file.size});
  while (input) {
    throw_if_cancelled(options.stop);
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    require(!input.bad() && (!input.fail() || input.eof()), "source file read failed");
    const auto count = static_cast<std::size_t>(input.gcount());
    require(count <= file.size - result.size(), "file changed during native text diff; retry operation");
    result.append(buffer.data(), count);
    report_progress(options, {ProgressPhase::verifying, file.relative, result.size(), file.size});
  }
  require(result.size() == file.size, "file changed during native text diff; retry operation");
  return result;
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
    difference = file_limit(before.size, after.size);
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
NativeWorkingDiff read_native_working_diff(const Store &store, const fs::path &directory,
                                           const std::optional<std::string> &source, std::string_view path,
                                           TextDiffOptions limits, OperationOptions options) {
  throw_if_cancelled(options.stop);
  limits.validate();
  validate_path(path);
  const auto base = fs::canonical(directory);
  auto before = side(store, source, path, options);
  const auto file = inspect_native_file(base, store.root(), path, options);
  require(!before.entry.is_null() || file, "native text diff path is absent from both snapshots");
  const auto size = file ? file->size : 0;
  const bool oversized = before.size > limits.max_file_bytes || size > limits.max_file_bytes;
  std::string content;
  if (!oversized)
    load(store, before, options);
  require(same_file(file, inspect_native_file(base, store.root(), path, options)),
          "file changed during native text diff; retry operation");
  if (file && !oversized)
    content = working_bytes(*file, options);
  require(fs::canonical(directory) == base &&
              same_file(file, inspect_native_file(base, store.root(), path, options)),
          "file changed during native text diff; retry operation");
  Json target;
  if (file) {
    target = Json{{"manifest", Json{}},
                  {"size", file->size},
                  {"file_hash", oversized ? Json{} : Json(hash(bytes(content)))},
                  {"executable", file->executable}};
#ifdef _WIN32
    target["executable"] = Json{};
#endif
  }
  const bool comparable =
#ifdef _WIN32
      false;
#else
      true;
#endif
  const bool mode_changed =
      comparable && !before.entry.is_null() && file && before.entry["executable"] != file->executable;
  Json changed;
  if (before.entry.is_null() || !file || before.size != size)
    changed = true;
  else if (!oversized)
    changed = before.entry["file_hash"] != target["file_hash"];
  Json equal;
  if (mode_changed || changed == true)
    equal = false;
  else if (changed == false)
    equal = true;
  auto difference = oversized ? file_limit(before.size, size)
                              : compare_text(bytes(before.content), bytes(content), limits, options);
  Json report{{"version", 1},
              {"path", path},
              {"comparison", "working"},
              {"directory", utf8(base)},
              {"source_revision", source ? Json(*source) : Json{}},
              {"target_revision", Json{}},
              {"source", before.entry},
              {"target", std::move(target)},
              {"equal", equal},
              {"content_changed", changed},
              {"executable_changed", mode_changed},
              {"executable_comparable", comparable},
              {"manifest_changed", Json{}},
              {"objects_verified", !oversized},
              {"workspace_verified", !oversized},
              {"diff", std::move(difference)}};
  require(report.dump().size() <= max_object_size, "native text diff report exceeds 16 MiB");
  report_progress(options, {ProgressPhase::planning, path, 1, 1, true});
  return {std::move(report),
          std::move(before.content),
          std::move(content),
          std::string(path),
          directory,
          base,
          file};
}
Json Store::diff_working_file(const fs::path &directory, const std::optional<std::string> &source,
                              std::string_view path, TextDiffOptions limits, OperationOptions options) const {
  return read_native_working_diff(*this, directory, source, path, limits, options).report;
}
void verify_native_working_diff(const Store &store, const NativeWorkingDiff &capture,
                                OperationOptions options) {
  require(fs::canonical(capture.requested) == capture.directory &&
              same_file(capture.file,
                        inspect_native_file(capture.directory, store.root(), capture.path, options)),
          "file changed during native text diff; retry operation");
}
} // namespace syrnck
