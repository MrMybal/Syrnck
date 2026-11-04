#include "internal.hpp"
#include <algorithm>
#include <exception>

namespace syrnck {
namespace {
fs::path session_directory(const Store &store, bool create = false) {
  auto directory = store.root() / "transfers";
  auto status = fs::symlink_status(directory);
  require(status.type() == fs::file_type::not_found || (fs::is_directory(status) && !fs::is_symlink(status)),
          "invalid transfer session directory");
  if (create && status.type() == fs::file_type::not_found) {
    require(fs::create_directory(directory), "cannot create transfer session directory");
    sync_directory(store.root());
  }
  return directory;
}
fs::path session_path(const Store &store, std::string_view id, bool create = false) {
  validate_id(id);
  return session_directory(store, create) / path_from_utf8(std::string(id) + ".json");
}
void check_file(const fs::file_status &status) {
  require(fs::is_regular_file(status) && !fs::is_symlink(status), "invalid transfer session file");
}
template <class Visitor> void visit_sessions(const Store &store, Visitor visitor, std::stop_token stop) {
  throw_if_cancelled(stop);
  auto directory = session_directory(store);
  if (!fs::exists(directory))
    return;
  for (const auto &file : fs::directory_iterator(directory)) {
    throw_if_cancelled(stop);
    auto name = utf8(file.path().filename());
    if (name.starts_with(".tmp"))
      continue; // An interrupted atomic write has not committed a record.
    const bool canonical = name.size() == 69 && name.ends_with(".json");
    if (!canonical && transient_metadata_disappeared(file.path(), stop))
      continue;
    check_file(file.symlink_status());
    require(canonical, "invalid transfer session file");
    auto session = store.transfer_session(std::string_view(name).substr(0, 64));
    require(session.has_value(), "transfer session disappeared");
    visitor(std::move(*session));
  }
  throw_if_cancelled(stop);
}
} // namespace
std::string TransferSession::identity(std::string_view direction, std::string_view endpoint,
                                      const ObjectKey &root, std::string_view reference) {
  return hash(bytes(Json::array({direction, endpoint, key_json(root), reference}).dump()));
}
void TransferSession::validate() const {
  require(direction == "push" || direction == "pull", "invalid transfer session direction");
  validate_id(endpoint);
  validate_ref(reference);
  require(direction != "push" || reference.find('/') == reference.npos,
          "remote reference must be a single component");
  (void)parse_key(key_json(root));
  if (expected)
    (void)parse_key(key_json(*expected));
  require(state == "transferring" || state == "publishing" || state == "uncertain" || state == "conflict" ||
              state == "complete",
          "invalid transfer session state");
  require(direction == "push" || state != "uncertain", "invalid transfer session state");
  if (upload_lease) {
    require(direction == "push", "only push sessions can hold an upload lease");
    validate_id(*upload_lease);
  }
  if (read_lease) {
    require(direction == "pull", "only pull sessions can hold a read lease");
    validate_id(*read_lease);
  }
  validate_id(id);
  require(id == identity(direction, endpoint, root, reference), "transfer session identity mismatch");
}
Json TransferSession::json() const {
  validate();
  return Json{{"version", 3},
              {"id", id},
              {"direction", direction},
              {"endpoint", endpoint},
              {"reference", reference},
              {"root", key_json(root)},
              {"expected", expected ? key_json(*expected) : Json(nullptr)},
              {"state", state},
              {"upload_lease", upload_lease ? Json(*upload_lease) : Json(nullptr)},
              {"read_lease", read_lease ? Json(*read_lease) : Json(nullptr)}};
}
TransferSession TransferSession::parse(const Json &value) {
  require(value.is_object() && value.contains("version") && value["version"].is_number_integer() &&
              ((value["version"] == 1 && value.size() == 8) ||
               ((value["version"] == 2 || value["version"] == 3) &&
                value.size() == (value["version"] == 2 ? 9 : 10) && value.contains("upload_lease") &&
                (value["upload_lease"].is_null() || value["upload_lease"].is_string()) &&
                (value["version"] == 2 ||
                 (value.contains("read_lease") &&
                  (value["read_lease"].is_null() || value["read_lease"].is_string()))))) &&
              value.contains("id") && value["id"].is_string() && value.contains("direction") &&
              value["direction"].is_string() && value.contains("endpoint") && value["endpoint"].is_string() &&
              value.contains("reference") && value["reference"].is_string() && value.contains("state") &&
              value["state"].is_string() && value.contains("root") && value.contains("expected"),
          "invalid transfer session document");
  TransferSession result;
  result.id = value["id"].get<std::string>();
  result.direction = value["direction"].get<std::string>();
  result.endpoint = value["endpoint"].get<std::string>();
  result.reference = value["reference"].get<std::string>();
  result.state = value["state"].get<std::string>();
  result.root = parse_key(value["root"]);
  if (!value["expected"].is_null())
    result.expected = parse_key(value["expected"]);
  if (value["version"] >= 2 && !value["upload_lease"].is_null())
    result.upload_lease = value["upload_lease"].get<std::string>();
  if (value["version"] == 3 && !value["read_lease"].is_null())
    result.read_lease = value["read_lease"].get<std::string>();
  result.validate();
  return result;
}
std::optional<TransferSession> Store::transfer_session(std::string_view id) const {
  auto path = session_path(*this, id);
  auto status = fs::symlink_status(path);
  if (status.type() == fs::file_type::not_found)
    return {};
  check_file(status);
  auto result = TransferSession::parse(parse_json(read_bytes(path, 16384)));
  require(result.id == id, "transfer session identity mismatch");
  return result;
}
std::vector<TransferSession> Store::transfer_sessions(std::stop_token stop) const {
  std::vector<TransferSession> result;
  visit_sessions(*this, [&](TransferSession session) { result.push_back(std::move(session)); }, stop);
  std::size_t comparisons = 0;
  std::sort(result.begin(), result.end(), [&](const auto &a, const auto &b) {
    if (++comparisons % 4096 == 0)
      throw_if_cancelled(stop);
    return a.id < b.id;
  });
  throw_if_cancelled(stop);
  return result;
}
void Store::transfer_gc_roots(std::vector<ObjectKey> &complete, std::vector<ObjectKey> &partial,
                              std::stop_token stop) const {
  visit_sessions(
      *this,
      [&](const TransferSession &session) {
        if (session.state != "complete")
          (session.direction == "push" ? complete : partial).push_back(session.root);
      },
      stop);
}
void Store::write_transfer_session(const TransferSession &value, std::stop_token stop) const {
  value.validate();
  throw_if_cancelled(stop);
  auto path = session_path(*this, value.id, true);
  auto status = fs::symlink_status(path);
  if (status.type() != fs::file_type::not_found)
    check_file(status);
  auto data = value.json().dump();
  require(data.size() <= 16384, "transfer session exceeds size limit");
  atomic_write(path, bytes(data), status.type() != fs::file_type::not_found, stop);
}
void Store::remove_transfer_session(std::string_view id) const {
  auto path = session_path(*this, id);
  auto status = fs::symlink_status(path);
  require(status.type() != fs::file_type::not_found, "unknown transfer session");
  check_file(status); // Explicit discard also permits deleting malformed records.
  require(fs::remove(path), "cannot remove transfer session");
  sync_directory(path.parent_path());
}
Json Store::prune_transfer_sessions(bool remove, OperationOptions options) const {
  std::size_t completed = 0, unfinished = 0, removed = 0;
  std::uint64_t scanned_bytes = 0, candidate_bytes = 0, removed_bytes = 0;
  report_progress(options, {ProgressPhase::planning, "transfers"});
  // Validate the entire inventory before the first deletion, without retaining every ID.
  visit_sessions(
      *this,
      [&](const TransferSession &session) {
        ++(session.state == "complete" ? completed : unfinished);
        auto size = fs::file_size(session_path(*this, session.id));
        scanned_bytes += size;
        if (session.state == "complete")
          candidate_bytes += size;
        report_progress(options, {ProgressPhase::planning, "transfers", scanned_bytes});
      },
      options.stop);
  report_progress(options, {ProgressPhase::planning, "transfers", scanned_bytes, scanned_bytes, true});
  if (remove) {
    report_progress(options, {ProgressPhase::publishing, "transfers", 0, candidate_bytes});
    try {
      visit_sessions(
          *this,
          [&](const TransferSession &session) {
            if (session.state == "complete") {
              auto path = session_path(*this, session.id);
              auto size = fs::file_size(path);
              require(fs::remove(path), "cannot remove transfer session");
              ++removed;
              removed_bytes += size;
              report_progress(options,
                              {ProgressPhase::publishing, "transfers", removed_bytes, candidate_bytes});
            }
          },
          options.stop);
    } catch (...) {
      auto original = std::current_exception();
      if (removed) {
        try {
          sync_directory(session_directory(*this));
        } catch (...) {
        }
      }
      std::rethrow_exception(original);
    }
    if (removed)
      sync_directory(session_directory(*this));
    report_progress(options, {ProgressPhase::publishing, "transfers", removed_bytes, candidate_bytes, true});
  }
  return Json{
      {"completed", completed}, {"unfinished", unfinished}, {"removed", removed}, {"deleted", remove}};
}
} // namespace syrnck
