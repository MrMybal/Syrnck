#include "native_replay.hpp"
namespace syrnck {
Json Store::replay_directory(const fs::path &directory, std::string_view branch,
                             std::string_view expected_revision, std::string_view source,
                             NativeReplayKind kind, std::string_view message,
                             std::optional<std::uint32_t> mainline, OperationOptions options) const {
  throw_if_cancelled(options.stop);
  validate_ref("heads/" + std::string(branch));
  validate_id(expected_revision);
  validate_id(source);
  require_no_native_checkout(*this);
  const auto all = refs();
  const auto found = all.find("heads/" + std::string(branch));
  require(found != all.end() && found->second.kind == Kind::revision, "unknown native branch");
  require(found->second.id == expected_revision, "native replay tip changed; inspect the branch and retry");
  require(!all.contains("staging/" + std::string(branch)), "native replay requires an empty staging index");
  auto plan = prepare_native_replay(*this, directory, branch, source, kind, mainline, true, options);
  require(plan.report["conflicts"].empty(), "native replay has conflicts; inspect the replay plan");
  require(!plan.report["empty"].get<bool>(), "native replay has no changes");
  require(plan.report["ready"].get<bool>(), "native replay is blocked; inspect the replay plan");
  Json files = Json::object();
  auto &object = files.get_ref<Json::object_t &>();
  object.reserve(plan.files.size());
  for (const auto &[path, file] : plan.files) {
    throw_if_cancelled(options.stop);
    object.emplace_back(path, Json{{"manifest", file.manifest}, {"executable", file.executable}});
  }
  const auto revision = Json{{"version", 1},
                             {"parents", Json::array({plan.head})},
                             {"message", message},
                             {"timestamp", unix_seconds()},
                             {"files", files}};
  throw_if_cancelled(options.stop);
  const auto next = put(Kind::revision, bytes(revision.dump())).first;
  auto result = checkout_directory(directory, branch, next.id, options);
  result["operation"] = kind == NativeReplayKind::revert ? "revert" : "cherry-pick";
  result["replayed_revision"] = source;
  result["previous_revision"] = plan.head;
  result["mainline"] = plan.report["mainline"];
  return result;
}
} // namespace syrnck
