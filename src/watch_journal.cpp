#include "watch_journal.hpp"
#include "native_workspace.hpp"
namespace syrnck {
std::unique_ptr<FileLock> lock_watch_journal(const fs::path &storage, std::string_view filename) {
  validate_ref(filename);
  require(filename.find('/') == filename.npos, "invalid native workspace local filename");
  const auto path = storage / path_from_utf8(filename);
  const auto status = fs::symlink_status(path);
  require(!fs::exists(status) || (fs::is_regular_file(status) && !native_link_like(path, status)),
          "invalid workspace watch lock");
  return std::make_unique<FileLock>(path);
}
void validate_watch_journal(const Json &value, OperationOptions options) {
  throw_if_cancelled(options.stop);
  try {
    require(value.is_object() && value.size() == 11 && value.at("version") == 1 &&
                value.at("type") == "native-watch-journal" && value.at("generation").is_number_unsigned() &&
                value.at("generation").get<std::uint64_t>() > 0, "invalid workspace watch journal");
    for (const auto *name : {"workspace", "session", "selection"})
      validate_id(value.at(name).get<std::string>());
    const auto backend = value.at("backend").get<std::string>();
    require(backend == "windows" || backend == "inotify" || backend == "polling", "invalid workspace watch journal");
    const auto state = value.at("state").get<std::string>();
    require(state == "pending" || state == "observed" || state == "unavailable", "invalid workspace watch journal");
    const auto &previous = value.at("previous");
    require(previous.is_object() && previous.size() == 2, "invalid workspace watch journal");
    const auto old_state = previous.at("state").get<std::string>();
    require(old_state == "missing" || old_state == "pending" || old_state == "observed" || old_state == "unavailable",
            "invalid workspace watch journal");
    if (old_state == "missing") require(previous.at("session").is_null(), "invalid workspace watch journal");
    else validate_id(previous.at("session").get<std::string>());
    const auto &batch = value.at("batch");
    require(batch.is_object() && batch.size() == 4 && batch.at("full_scan").is_boolean() &&
                batch.at("overflow").is_boolean() && batch.at("polling").is_boolean() &&
                batch.at("paths").is_array() && batch["paths"].size() <= 65536, "invalid workspace watch journal");
    require(!batch["overflow"].get<bool>() || (batch["full_scan"].get<bool>() && batch["paths"].empty()),
            "invalid workspace watch journal");
    std::size_t budget = 0;
    std::string last;
    for (const auto &row : batch["paths"]) {
      throw_if_cancelled(options.stop);
      const auto path = row.get<std::string>();
      // Hints may name protected refs or ignored names that are not valid
      // repository paths. Validate relative filesystem components separately.
      require(!path.empty() && is_utf8_text(bytes(path), options) && path.front() != '/' &&
                  (last.empty() || last < path), "invalid workspace watch journal");
      for (std::size_t start = 0; start < path.size();) {
        const auto slash = path.find('/', start);
        const auto part = std::string_view(path).substr(start, slash == path.npos ? path.size() - start : slash - start);
        require(!part.empty() && part != "." && part != "..", "invalid workspace watch journal");
        if (slash == path.npos) break;
        start = slash + 1;
        require(start < path.size(), "invalid workspace watch journal");
      }
      require(path.size() + 64 <= 1024 * 1024 - budget, "workspace watch journal paths exceed 1 MiB");
      budget += path.size() + 64;
      last = path;
    }
    const auto &observation = value.at("observation");
    if (state != "observed") require(observation.is_null(), "invalid workspace watch journal");
    else {
      require(observation.is_object() && observation.size() == 4 && observation.at("clean").is_boolean() &&
                  observation.at("workspace_files").is_number_unsigned(), "invalid workspace watch journal");
      validate_id(observation.at("digest").get<std::string>());
      if (!observation.at("base").is_null()) validate_id(observation["base"].get<std::string>());
    }
    require(value.dump().size() <= watch_journal_limit, "workspace watch journal exceeds 2 MiB");
  } catch (const Json::exception &) { throw Error("invalid workspace watch journal"); }
}
} // namespace syrnck
