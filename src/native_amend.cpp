#include "native_workspace.hpp"
#include "native_context.hpp"
namespace syrnck {
namespace {
std::optional<ObjectKey> lookup(const Refs &refs, const std::string &name) {
  const auto found = refs.find(name);
  return found == refs.end() ? std::optional<ObjectKey>{} : std::optional{found->second};
}
struct Amendment {
  std::string head_name, stage_name, archive_name;
  ObjectKey previous;
  std::optional<ObjectKey> index, archive;
  Json revision;
  Amendment(const Store &store, std::string_view branch, std::string_view expected, bool staged,
            std::stop_token stop) {
    require_native_branch_available(store, branch);
    throw_if_cancelled(stop);
    validate_native_target(branch);
    validate_id(expected);
    require_no_native_checkout(store);
    head_name = native_revision_reference(store, std::string(branch));
    stage_name = native_staging_reference(store, std::string(branch));
    archive_name = "archive/amend/" + std::string(expected);
    validate_ref(head_name);
    validate_ref(stage_name);
    const auto refs = store.refs();
    const auto head = lookup(refs, head_name);
    require(head && head->kind == Kind::revision, "unknown native branch");
    previous = *head;
    require(previous.id == expected, "native amendment tip changed; inspect the branch and retry");
    index = lookup(refs, stage_name);
    archive = lookup(refs, archive_name);
    require(!archive || *archive == previous, "native amendment archive points to another object");
    revision = store.metadata(Kind::revision, previous.id);
    if (staged) {
      require(index.has_value(), "no native staging snapshot");
      require(index->kind == Kind::revision, "invalid native staging snapshot");
      const auto prepared = store.metadata(Kind::revision, index->id);
      require(prepared["message"] == "Syrnck staging v1: " + std::string(branch) &&
                  prepared["timestamp"] == 0 && prepared["parents"].size() <= 1,
              "invalid native staging snapshot");
      require(prepared["parents"] == Json::array({previous.id}),
              "native staging baseline changed; inspect or reset the staging index");
      revision["files"] = prepared["files"];
    } else {
      require(!index, "native staging exists; commit with --staged or reset the staging index");
    }
  }
  std::string publish(const Store &store, std::string_view message, OperationOptions options) {
    throw_if_cancelled(options.stop);
    revision["message"] = message;
    revision["timestamp"] = unix_seconds();
    const auto next = store.put(Kind::revision, bytes(revision.dump())).first;
    require(store.compare_update_refs(
                {{head_name, previous, next}, {stage_name, index, {}}, {archive_name, archive, previous}},
                options),
            "native amendment references changed; retry operation");
    return next.id;
  }
};
} // namespace
std::string Store::amend_directory(const fs::path &directory, std::string_view branch,
                                   std::string_view expected, std::string_view message) const {
  return amend_directory(directory, branch, expected, message, settings().ingest_options());
}
std::string Store::amend_directory(const fs::path &directory, std::string_view branch,
                                   std::string_view expected, std::string_view message,
                                   IngestOptions options) const {
  Amendment amendment(*this, branch, expected, false, options.stop);
  std::map<std::string, Json> files;
  visit_native_files(directory, *this,
                     [&](const NativeFile &file) {
                       files.emplace(file.relative,
                                     ingest_native_file(*this, file, options,
                                                        "file changed during ingestion; retry commit"));
                     },
                     {options.stop, options.progress});
  Json entries = Json::object();
  auto &object = entries.get_ref<Json::object_t &>();
  object.reserve(files.size());
  for (auto &[name, value] : files)
    object.emplace_back(name, std::move(value));
  amendment.revision["files"] = std::move(entries);
  return amendment.publish(*this, message, {options.stop, options.progress});
}
std::string Store::amend_staged(std::string_view branch, std::string_view expected, std::string_view message,
                                OperationOptions options) const {
  Amendment amendment(*this, branch, expected, true, options.stop);
  return amendment.publish(*this, message, options);
}
std::string Store::amend_message(std::string_view branch, std::string_view expected, std::string_view message,
                                 OperationOptions options) const {
  Amendment amendment(*this, branch, expected, false, options.stop);
  return amendment.publish(*this, message, options);
}
} // namespace syrnck
