#include "internal.hpp"
namespace syrnck {
Json Store::compare_files(std::string_view source_id, std::string_view target_id,
                          OperationOptions options) const {
  throw_if_cancelled(options.stop);
  const auto source = metadata(Kind::manifest, source_id);
  const auto other = source_id == target_id ? Json{} : metadata(Kind::manifest, target_id);
  const auto &target = source_id == target_id ? source : other;
  struct Position {
    std::uint64_t offset, size;
  };
  std::map<std::string, Position> reusable;
  const auto total = source["blocks"].size() + target["blocks"].size();
  std::uint64_t processed = 0, offset = 0;
  auto notify = [&](bool complete = false) {
    report_progress(options, {ProgressPhase::planning, target_id, processed, total, complete});
  };
  notify();
  for (const auto &block : source["blocks"]) {
    throw_if_cancelled(options.stop);
    auto size = block["size"].get<std::uint64_t>();
    auto [at, inserted] = reusable.emplace(block["id"].get<std::string>(), Position{offset, size});
    require(inserted || at->second.size == size, "inconsistent file comparison block size");
    offset += size;
    ++processed;
    if (processed % 1024 == 0)
      notify();
  }
  Json blocks = Json::array(), needed = Json::array();
  std::map<std::string, std::uint64_t> missing;
  std::uint64_t reused_bytes = 0, new_bytes = 0, unique_new_bytes = 0;
  offset = 0;
  for (const auto &block : target["blocks"]) {
    throw_if_cancelled(options.stop);
    const auto id = block["id"].get<std::string>();
    const auto size = block["size"].get<std::uint64_t>();
    Json operation{{"id", id}, {"offset", offset}, {"size", size}, {"source_offset", nullptr}};
    if (auto found = reusable.find(id); found != reusable.end()) {
      require(found->second.size == size, "inconsistent file comparison block size");
      operation["source_offset"] = found->second.offset;
      reused_bytes += size;
    } else {
      auto [at, inserted] = missing.emplace(id, size);
      require(inserted || at->second == size, "inconsistent file comparison block size");
      new_bytes += size;
      if (inserted) {
        unique_new_bytes += size;
        needed.push_back(Json{{"id", id}, {"size", size}});
      }
    }
    blocks.push_back(std::move(operation));
    offset += size;
    ++processed;
    if (processed % 1024 == 0)
      notify();
  }
  Json result{
      {"version", 1},
      {"source", source_id},
      {"target", target_id},
      {"source_size", source["size"]},
      {"target_size", target["size"]},
      {"same_content", source["size"] == target["size"] && source["file_hash"] == target["file_hash"]},
      {"reused_bytes", reused_bytes},
      {"new_bytes", new_bytes},
      {"unique_new_bytes", unique_new_bytes},
      {"blocks", std::move(blocks)},
      {"needed_chunks", std::move(needed)}};
  notify(true);
  return result;
}
} // namespace syrnck
