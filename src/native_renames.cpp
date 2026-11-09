#include "native_renames.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <sstream>
namespace syrnck {
namespace {
// Compare exact byte fractions without floating-point rounding or products
// that could overflow for large assets. Continued fractions need only division.
int compare_fraction(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d) {
  int direction = 1;
  for (;;) {
    const auto left = a / b, right = c / d;
    if (left != right)
      return left < right ? -direction : direction;
    a %= b;
    c %= d;
    if (!a || !c)
      return a == c ? 0 : !a ? -direction : direction;
    std::swap(a, b);
    std::swap(c, d);
    direction = -direction;
  }
}
} // namespace
void NativeRenameOptions::validate() const {
  require(min_similarity >= 1 && min_similarity <= 100, "rename similarity must be in 1..100");
  require(max_candidates >= 1 && max_candidates <= 16384, "rename candidate limit must be in 1..16384");
  require(max_work >= 1 && max_work <= 16000000, "rename work limit must be in 1..16000000");
  require(max_input_bytes >= 1024 && max_input_bytes <= 256 * 1024 * 1024,
          "rename input budget must be in 1024..268435456 bytes");
}
NativeRenameBatch::NativeRenameBatch(const Store &store, const NativeRenameOptions &settings,
                                     const NativeGeneratedText *generated,
                                     std::function<void(std::size_t)> observer)
    : limits(settings), store_(store), generated_(generated), input_observer_(std::move(observer)) {
  limits.validate();
}
void NativeRenameBatch::input(std::size_t count) {
  if (count > limits.max_input_bytes - input_bytes_)
    throw NativeRenameLimit("native rename similarity exceeds input budget");
  if (input_observer_)
    input_observer_(count);
  input_bytes_ += count;
}
void NativeRenameBatch::work(OperationOptions options) {
  throw_if_cancelled(options.stop);
  if (work_ == limits.max_work)
    throw NativeRenameLimit("native rename similarity exceeds work budget");
  ++work_;
}
const NativeRenameBatch::Features &NativeRenameBatch::features(const NativeSnapshotFile &file,
                                                               OperationOptions options) {
  throw_if_cancelled(options.stop);
  if (const auto found = cache_.find(file.manifest); found != cache_.end())
    return found->second;
  if (cache_.size() == limits.max_candidates)
    throw NativeRenameLimit("native rename similarity exceeds candidate limit");
  Features result;
  std::string text;
  const auto generated = generated_ ? generated_->find(file.manifest) : NativeGeneratedText::const_iterator{};
  if (generated_ && generated != generated_->end()) {
    input(generated->second.size());
    text = generated->second;
    auto remaining = bytes(text);
    while (!remaining.empty()) {
      work(options);
      const auto size = chunk_cut(remaining, Chunking{});
      result["b" + object_id(Kind::chunk, remaining.first(size))] += size;
      remaining = remaining.subspan(size);
    }
  } else {
    auto manifest = store_.metadata(Kind::manifest, file.manifest);
    input(manifest.dump().size());
    require(manifest["size"] == file.content.size && manifest["file_hash"] == file.content.hash,
            "native rename manifest summary changed");
    for (const auto &block : manifest["blocks"]) {
      work(options);
      result["b" + block["id"].get<std::string>()] += block["size"].get<std::uint64_t>();
    }
    // Whole-file reconstruction is bounded, verified, and only needed to find
    // edits within a native block (common for small source files).
    if (file.content.size && file.content.size <= 256 * 1024) {
      input(static_cast<std::size_t>(file.content.size));
      std::ostringstream output(std::ios::binary);
      store_.reconstruct(file.manifest, output, options);
      text = std::move(output).str();
    }
  }
  if (!text.empty()) {
    require(text.size() == file.content.size && hash(bytes(text)) == file.content.hash,
            "reconstructed file hash mismatch");
    if (text.size() <= 256 * 1024 && is_utf8_text(bytes(text), options)) {
      for (std::size_t pos = 0; pos < text.size();) {
        work(options);
        const auto newline = text.find('\n', pos);
        const auto end =
            std::min(text.size(), std::min(pos + 64, newline == text.npos ? text.size() : newline + 1));
        const auto span = bytes(std::string_view(text).substr(pos, end - pos));
        result["t" + hash(span)] += span.size();
        pos = end;
      }
    }
  }
  return cache_.emplace(file.manifest, std::move(result)).first->second;
}
Json NativeRenameBatch::report() const {
  return Json{{"mode", "similarity"},         {"min_similarity", limits.min_similarity},
              {"candidates", cache_.size()},  {"work", work_},
              {"input_bytes", input_bytes_},  {"max_candidates", limits.max_candidates},
              {"max_work", limits.max_work},  {"max_input_bytes", limits.max_input_bytes},
              {"text_file_limit", 256 * 1024}};
}
NativeRenameResult match_native_renames(const NativeSnapshot &source, const NativeSnapshot &target,
                                        NativeRenameBatch *batch, OperationOptions options) {
  using Identity = std::pair<std::uint64_t, std::string>;
  struct Group {
    std::vector<std::string> removed, added;
  };
  std::map<Identity, Group> groups;
  for (const auto &[path, file] : source) {
    throw_if_cancelled(options.stop);
    if (!target.contains(path))
      groups[{file.content.size, file.content.hash}].removed.push_back(path);
  }
  for (const auto &[path, file] : target) {
    throw_if_cancelled(options.stop);
    if (!source.contains(path))
      groups[{file.content.size, file.content.hash}].added.push_back(path);
  }
  NativeRenameResult result;
  std::set<std::string> matched_targets;
  for (const auto &[identity, group] : groups) {
    (void)identity;
    throw_if_cancelled(options.stop);
    if (group.removed.size() == 1 && group.added.size() == 1) {
      result.matches.emplace(group.removed[0], NativeRenameMatch{group.added[0], 100});
      matched_targets.insert(group.added[0]);
    } else if (!group.removed.empty() && !group.added.empty()) {
      result.ambiguous_sources.insert(group.removed.begin(), group.removed.end());
      result.ambiguous_targets.insert(group.added.begin(), group.added.end());
    }
  }
  if (batch) {
    const bool have_old = std::any_of(source.begin(), source.end(), [&](const auto &entry) {
      return !target.contains(entry.first) && !result.matches.contains(entry.first) &&
             !result.ambiguous_sources.contains(entry.first);
    });
    const bool have_new = std::any_of(target.begin(), target.end(), [&](const auto &entry) {
      return !source.contains(entry.first) && !matched_targets.contains(entry.first) &&
             !result.ambiguous_targets.contains(entry.first);
    });
    if (have_old && have_new) {
      using Posting = std::pair<std::string, std::uint64_t>;
      std::map<std::string, std::vector<Posting>> index;
      struct Best {
        unsigned score = 0;
        std::uint64_t shared = 0, total = 0;
        std::string path;
        bool tied = false;
        void offer(std::uint64_t common, std::uint64_t size, unsigned value, const std::string &candidate) {
          const auto order = total ? compare_fraction(common, size, shared, total) : 1;
          if (order > 0) {
            score = value;
            shared = common;
            total = size;
            path = candidate;
            tied = false;
          } else if (order == 0)
            tied = true;
        }
      };
      std::map<std::string, Best> old_best, new_best;
      for (const auto &[path, file] : source) {
        if (target.contains(path) || result.matches.contains(path) || result.ambiguous_sources.contains(path))
          continue;
        for (const auto &[feature, weight] : batch->features(file, options)) {
          batch->work(options);
          index[feature].emplace_back(path, weight);
        }
      }
      for (const auto &[path, file] : target) {
        if (source.contains(path) || matched_targets.contains(path) ||
            result.ambiguous_targets.contains(path))
          continue;
        std::map<std::string, std::array<std::uint64_t, 2>> shared;
        for (const auto &[feature, weight] : batch->features(file, options)) {
          batch->work(options);
          if (const auto found = index.find(feature); found != index.end())
            for (const auto &[old, old_weight] : found->second) {
              batch->work(options);
              shared[old][feature[0] == 't' ? 1 : 0] += std::min(weight, old_weight);
            }
        }
        for (const auto &[old, weights] : shared) {
          batch->work(options);
          const auto total = std::max(file.content.size, source.at(old).content.size);
          const auto common = std::max(weights[0], weights[1]);
          require(common <= total && common <= std::numeric_limits<std::uint64_t>::max() / 100,
                  "native rename similarity size exceeds scoring limit");
          const auto score = total ? static_cast<unsigned>(common * 100 / total) : 0;
          if (score >= batch->limits.min_similarity) {
            old_best[old].offer(common, total, score, path);
            new_best[path].offer(common, total, score, old);
          }
        }
      }
      for (const auto &[old, best] : old_best) {
        const auto &other = new_best.at(best.path);
        if (!best.tied && !other.tied && other.path == old) {
          result.matches.emplace(old, NativeRenameMatch{best.path, best.score});
          matched_targets.insert(best.path);
        } else
          result.ambiguous_sources.insert(old);
      }
      for (const auto &[path, best] : new_best) {
        (void)best;
        if (!matched_targets.contains(path))
          result.ambiguous_targets.insert(path);
      }
    }
    result.report = batch->report();
  } else
    result.report = Json{{"mode", "exact"}};
  result.report["ambiguous_sources"] = result.ambiguous_sources.size();
  result.report["ambiguous_targets"] = result.ambiguous_targets.size();
  return result;
}
} // namespace syrnck
