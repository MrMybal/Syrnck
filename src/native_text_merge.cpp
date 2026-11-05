#include "native_text_merge.hpp"
#include <algorithm>
#include <sstream>
namespace syrnck {
std::string native_text_manifest(std::string_view content, OperationOptions options) {
  require(content.size() <= 4 * 1024 * 1024, "native generated text exceeds 4 MiB");
  const Chunking chunking;
  Json blocks = Json::array();
  auto remaining = bytes(content);
  while (!remaining.empty()) {
    throw_if_cancelled(options.stop);
    const auto size = chunk_cut(remaining, chunking);
    require(size && size <= remaining.size(), "invalid chunker boundary");
    blocks.push_back(Json{{"id", object_id(Kind::chunk, remaining.first(size))}, {"size", size}});
    remaining = remaining.subspan(size);
  }
  throw_if_cancelled(options.stop);
  const auto value = Json{{"version", 1},
                          {"size", content.size()},
                          {"file_hash", hash(bytes(content))},
                          {"chunking", chunking.json()},
                          {"blocks", std::move(blocks)}};
  return object_id(Kind::manifest, bytes(value.dump()));
}
void materialize_native_text(const Store &store, const NativeGeneratedText &files, OperationOptions options) {
  auto ingestion = store.settings().ingest_options();
  ingestion.stop = options.stop;
  for (const auto &[manifest, content] : files) {
    throw_if_cancelled(options.stop);
    ingestion.progress = [&](const ProgressEvent &event) {
      auto contextual = event;
      contextual.path = manifest;
      report_progress(options, contextual);
    };
    std::istringstream input(content);
    const auto actual = store.ingest(input, {}, ingestion).first;
    require(actual == manifest, "native generated text manifest changed; refresh the plan");
  }
}
NativeTextMergeBatch::NativeTextMergeBatch(const Store &store, TextDiffOptions limits)
    : store_(store), limits_(limits) {
  limits_.validate();
}
std::optional<NativeSnapshotFile> NativeTextMergeBatch::merge(std::string_view path,
                                                              const NativeSnapshotFile &base,
                                                              const NativeSnapshotFile &current,
                                                              const NativeSnapshotFile &incoming,
                                                              OperationOptions options) {
  throw_if_cancelled(options.stop);
  Json row{{"path", path},
           {"status", "limited"},
           {"reason", Json{}},
           {"conflicts", Json::array()},
           {"target", Json{}}};
  std::optional<NativeSnapshotFile> result;
  if (attempted_ == max_files) {
    row["reason"] = "file_count_limit";
  } else {
    ++attempted_;
    if (base.content.size > limits_.max_file_bytes || current.content.size > limits_.max_file_bytes ||
        incoming.content.size > limits_.max_file_bytes) {
      row["reason"] = "file_limit";
    } else {
      const auto cost =
          static_cast<std::size_t>(base.content.size + current.content.size + incoming.content.size);
      if (cost > max_total_bytes - input_bytes_ - result_bytes_) {
        row["reason"] = "total_bytes_limit";
      } else {
        input_bytes_ += cost;
        auto read = [&](const NativeSnapshotFile &file) {
          std::ostringstream output(std::ios::binary);
          store_.reconstruct(file.manifest, output, options);
          auto content = std::move(output).str();
          require(content.size() == file.content.size && hash(bytes(content)) == file.content.hash,
                  "reconstructed file hash mismatch");
          return content;
        };
        const auto original = read(base), ours = read(current), theirs = read(incoming);
        auto merged = merge_text(bytes(original), bytes(ours), bytes(theirs), limits_, options);
        row["status"] = merged["status"];
        row["reason"] = merged["reason"];
        row["conflicts"] = std::move(merged["conflicts"]);
        if (row["status"] == "clean") {
          auto content = merged["content"].get<std::string>();
          if (content.size() > max_total_bytes - input_bytes_ - result_bytes_) {
            row["status"] = "limited";
            row["reason"] = "total_bytes_limit";
          } else {
            result_bytes_ += content.size();
            const NativeContent logical{content.size(), hash(bytes(content))};
            auto manifest = logical == current.content    ? current.manifest
                            : logical == incoming.content ? incoming.manifest
                            : logical == base.content     ? base.manifest
                                                          : native_text_manifest(content, options);
            result = NativeSnapshotFile{manifest, logical,
                                        current.executable == base.executable ? incoming.executable
                                                                              : current.executable};
            if (logical != current.content && logical != incoming.content && logical != base.content)
              generated.try_emplace(manifest, std::move(content));
            row["target"] = result->json();
          }
        }
      }
    }
  }
  const auto cost = row.dump().size() + 1;
  require(cost <= max_object_size - report_bytes_, "native text merge report exceeds 16 MiB");
  report_bytes_ += cost;
  rows_.push_back(std::move(row));
  return result;
}
Json NativeTextMergeBatch::report() const {
  return Json{{"enabled", true},
              {"max_files", max_files},
              {"max_total_bytes", max_total_bytes},
              {"limits", Json{{"max_file_bytes", limits_.max_file_bytes},
                              {"max_lines", limits_.max_lines},
                              {"max_work", limits_.max_work},
                              {"max_output_bytes", limits_.max_output_bytes}}},
              {"attempted_files", attempted_},
              {"input_bytes", input_bytes_},
              {"result_bytes", result_bytes_},
              {"generated_manifests", generated.size()},
              {"files", rows_}};
}
} // namespace syrnck
