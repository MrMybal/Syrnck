#include "text_edits.hpp"
#include <syrnck/history.hpp>
#include "native_authorship.hpp"
#include "native_renames.hpp"
#include <algorithm>
#include <limits>
#include <sstream>
namespace syrnck {
namespace {
using text_detail::Limited;
struct Content {
  std::string value;
  text_detail::Lines lines;
};
struct Origin {
  std::string revision, path;
  std::size_t line = 0;
};
struct Pending {
  std::string revision, path;
  // Head line index -> line index in this ancestor.
  std::vector<std::pair<std::size_t, std::size_t>> lines;
};
class Attribution {
  const Store &store_;
  const NativeBlameOptions &limits_;
  const OperationOptions &options_;
  std::map<ObjectKey, Json> metadata_;
  std::map<std::string, Content> contents_;
  std::optional<NativeRenameBatch> renames_;
public:
  std::size_t revisions = 0, input_bytes = 0;
  text_detail::Work work;
  Attribution(const Store &store, const NativeBlameOptions &limits, const OperationOptions &options)
      : store_(store), limits_(limits), options_(options), work{limits.text.max_work, options} {
    if (limits.renames)
      renames_.emplace(store, *limits.renames, nullptr, [this](std::size_t bytes) { input(bytes); });
  }
  Json rename_report() const { return renames_ ? renames_->report() : Json{}; }
  void input(std::size_t bytes) {
    if (bytes > limits_.max_input_bytes - input_bytes)
      throw Limited{"input_limit"};
    input_bytes += bytes;
  }
  const Json &metadata(Kind kind, const std::string &id) {
    throw_if_cancelled(options_.stop);
    const ObjectKey key{kind, id};
    if (const auto found = metadata_.find(key); found != metadata_.end())
      return found->second;
    if (kind == Kind::revision && revisions == limits_.max_revisions)
      throw Limited{"revision_limit"};
    auto value = store_.metadata(kind, id);
    input(value.dump().size());
    if (kind == Kind::revision)
      ++revisions;
    return metadata_.emplace(key, std::move(value)).first->second;
  }
  const Json &manifest(const Json &file) {
    return metadata(Kind::manifest, file["manifest"].get<std::string>());
  }
  const Content &content(const Json &file) {
    const auto id = file["manifest"].get<std::string>();
    if (const auto found = contents_.find(id); found != contents_.end())
      return found->second;
    const auto size = manifest(file)["size"].get<std::uint64_t>();
    if (size > limits_.text.max_file_bytes)
      throw Limited{"file_limit"};
    input(static_cast<std::size_t>(size));
    std::ostringstream output(std::ios::binary);
    store_.reconstruct(id, output, options_);
    auto &value = contents_[id];
    value.value = std::move(output).str();
    value.lines = text_detail::split(bytes(value.value), limits_.text.max_lines, options_);
    return value;
  }
  bool equal_content(const Json &a, const Json &b) {
    work.use();
    if (a["manifest"] == b["manifest"])
      return true;
    const auto &left = manifest(a), &right = manifest(b);
    return left["size"] == right["size"] && left["file_hash"] == right["file_hash"];
  }
  std::optional<std::string> parent_path(const Json &current, const Json &parent, const std::string &path) {
    if (parent["files"].contains(path))
      return path;
    if (!limits_.follow_renames)
      return {};
    if (renames_) {
      NativeSnapshot removed, added;
      const auto decode = [&](const Json &entry) {
        const auto &value = manifest(entry);
        return NativeSnapshotFile{entry["manifest"].get<std::string>(),
          {value["size"].get<std::uint64_t>(), value["file_hash"].get<std::string>()},
          entry["executable"].get<bool>()};
      };
      for (const auto &[name, entry] : parent["files"].items()) {
        work.use();
        if (!current["files"].contains(name)) removed.emplace(name, decode(entry));
      }
      for (const auto &[name, entry] : current["files"].items()) {
        work.use();
        if (!parent["files"].contains(name)) added.emplace(name, decode(entry));
      }
      const auto matches = match_native_renames(removed, added, &*renames_, options_);
      for (const auto &[old, match] : matches.matches)
        if (match.target == path) return old;
      return {};
    }
    const auto &file = current["files"][path];
    std::optional<std::string> candidate;
    for (const auto &[name, entry] : parent["files"].items()) {
      work.use();
      if (!current["files"].contains(name) && equal_content(file, entry)) {
        if (candidate)
          return {}; // Two removed sources have identical bytes.
        candidate = name;
      }
    }
    if (candidate)
      for (const auto &[name, entry] : current["files"].items()) {
        work.use();
        if (name != path && !parent["files"].contains(name) && equal_content(file, entry))
          return {}; // Multiple added destinations are ambiguous too.
      }
    return candidate;
  }
  std::vector<Origin> trace(const std::string &revision, const std::string &path, const Content &head) {
    std::vector<Origin> origins(head.lines.size());
    Pending start{revision, path, {}};
    for (std::size_t i = 0; i < head.lines.size(); ++i)
      start.lines.emplace_back(i, i);
    std::vector<Pending> pending;
    pending.push_back(std::move(start));
    while (!pending.empty()) {
      throw_if_cancelled(options_.stop);
      auto current = std::move(pending.back());
      pending.pop_back();
      const auto &record = metadata(Kind::revision, current.revision);
      const auto &file = content(record["files"][current.path]);
      const auto &parents = record["parents"];
      for (std::size_t i = 0; i < parents.size() && !current.lines.empty(); ++i) {
        if (limits_.first_parent && i)
          break;
        const auto parent_id = parents[i].get<std::string>();
        const auto &parent = metadata(Kind::revision, parent_id);
        const auto old_path = parent_path(record, parent, current.path);
        if (!old_path)
          continue;
        const auto &old = content(parent["files"][*old_path]);
        if (!is_utf8_text(bytes(old.value), options_))
          continue; // Text introduced over binary content originates here.
        const auto edits = text_detail::edits(old.lines, file.lines, work);
        const auto absent = std::numeric_limits<std::size_t>::max();
        std::vector<std::size_t> inherited(file.lines.size(), absent);
        std::size_t before = 0, after = 0;
        for (const auto &edit : edits) {
          if (edit.kind == ' ')
            inherited[after] = before;
          if (edit.kind != '+')
            ++before;
          if (edit.kind != '-')
            ++after;
        }
        Pending older{parent_id, *old_path, {}};
        std::vector<std::pair<std::size_t, std::size_t>> remaining;
        for (const auto &[target, source] : current.lines) {
          work.use();
          require(source < inherited.size(), "invalid native blame line mapping");
          if (inherited[source] != absent)
            older.lines.emplace_back(target, inherited[source]);
          else
            remaining.emplace_back(target, source);
        }
        current.lines = std::move(remaining);
        if (!older.lines.empty())
          pending.push_back(std::move(older));
      }
      for (const auto &[target, source] : current.lines)
        origins[target] = Origin{current.revision, current.path, source + 1};
      report_progress(options_, {ProgressPhase::planning, current.path, revisions});
    }
    return origins;
  }
};
} // namespace
Json native_blame(const Store &store, std::string_view revision, std::string_view path,
                   NativeBlameOptions limits, OperationOptions options) {
  throw_if_cancelled(options.stop);
  validate_id(revision);
  validate_path(path);
  limits.text.validate();
  require(limits.follow_renames || !limits.renames, "rename similarity requires rename following");
  require(limits.max_revisions >= 1 && limits.max_revisions <= 10000,
          "native blame traversal limit must be in 1..10000");
  require(limits.max_input_bytes >= 1 && limits.max_input_bytes <= 256 * 1024 * 1024,
          "native blame input budget must be in 1..268435456 bytes");
  Attribution analysis(store, limits, options);
  Json report{{"version", 1}, {"revision", revision}, {"path", path}, {"status", "complete"},
              {"reason", Json{}}, {"traversal", limits.first_parent ? "first-parent" : "all-parents"},
              {"follow_renames", limits.follow_renames}, {"lines", Json::array()},
              {"commits", Json::object()}};
  require(report.dump().size() + 512 <= limits.text.max_output_bytes,
          "native blame query exceeds output budget");
  try {
    const auto &record = analysis.metadata(Kind::revision, std::string(revision));
    require(record["files"].contains(std::string(path)), "native blame path is absent from revision");
    const auto &head = analysis.content(record["files"][std::string(path)]);
    if (!is_utf8_text(bytes(head.value), options)) {
      report["status"] = "binary";
      report["reason"] = "non_text";
    } else {
      const auto origins = analysis.trace(std::string(revision), std::string(path), head);
      std::size_t output_bytes = report.dump().size() + 512;
      for (std::size_t i = 0; i < origins.size(); ++i) {
        throw_if_cancelled(options.stop);
        const auto &origin = origins[i];
        require(!origin.revision.empty(), "incomplete native blame attribution");
        if (!report["commits"].contains(origin.revision)) {
          const auto &commit = analysis.metadata(Kind::revision, origin.revision);
          auto message = commit["message"].get<std::string>();
          auto end = std::min<std::size_t>(256, message.size());
          while (end < message.size() && end && (static_cast<unsigned char>(message[end]) & 0xc0) == 0x80)
            --end;
          Json summary{{"timestamp", commit["timestamp"]}, {"message", message.substr(0, end)},
                       {"message_truncated", end < message.size()}};
          append_native_authorship(summary, commit);
          output_bytes += summary.dump().size() + origin.revision.size() + 8;
          if (output_bytes > limits.text.max_output_bytes)
            throw Limited{"output_limit"};
          report["commits"][origin.revision] = std::move(summary);
        }
        Json row{{"line", i + 1}, {"revision", origin.revision}, {"path", origin.path},
                 {"source_line", origin.line}, {"content", head.lines[i]}};
        output_bytes += row.dump().size() + 1;
        if (output_bytes > limits.text.max_output_bytes)
          throw Limited{"output_limit"};
        report["lines"].push_back(std::move(row));
      }
    }
  } catch (const NativeRenameLimit &) {
    report["status"] = "limited";
    report["reason"] = "rename_limit";
    report["lines"] = Json::array();
    report["commits"] = Json::object();
  } catch (const Limited &limit) {
    report["status"] = "limited";
    report["reason"] = limit.reason;
    report["lines"] = Json::array();
    report["commits"] = Json::object();
  }
  if (limits.renames) report["rename_detection"] = analysis.rename_report();
  report["visited_revisions"] = analysis.revisions;
  report["input_bytes"] = analysis.input_bytes;
  report["work"] = limits.text.max_work - analysis.work.remaining;
  report_progress(options, {ProgressPhase::planning, {}, analysis.revisions, analysis.revisions, true});
  return report;
}
} // namespace syrnck
