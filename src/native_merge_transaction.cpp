#include "native_merge.hpp"
namespace syrnck {
Json Store::merge_directory(const fs::path &directory, std::string_view branch,
                            std::string_view expected_revision, std::string_view source,
                            std::string_view message, const NativeMergeOptions &settings,
                            OperationOptions options) const {
  throw_if_cancelled(options.stop);
  validate_ref("heads/" + std::string(branch));
  validate_id(expected_revision);
  validate_id(source);
  require_no_native_checkout(*this);
  const auto all = refs();
  const auto head = all.find("heads/" + std::string(branch));
  require(head != all.end() && head->second.kind == Kind::revision, "unknown native branch");
  require(head->second.id == expected_revision, "native merge tip changed; inspect the branch and retry");
  require(!all.contains("staging/" + std::string(branch)), "native merge requires an empty staging index");
  auto plan = prepare_native_merge(*this, directory, branch, source, settings, true, options);
  require(plan.report["ready"].get<bool>(), "native merge is blocked; inspect native merge-plan");
  const auto mode = plan.report["mode"].get<std::string>();
  std::string target(source);
  Json result;
  if (mode == "up_to_date") {
    target = plan.head;
    result = Json{{"version", 1}, {"pending", false}, {"state", "unchanged"}, {"target", target}};
  } else {
    if (mode == "merge") {
      materialize_native_text(*this, plan.generated, options);
      Json files = Json::object();
      auto &entries = files.get_ref<Json::object_t &>();
      entries.reserve(plan.files.size());
      for (const auto &[path, file] : plan.files) {
        throw_if_cancelled(options.stop);
        entries.emplace_back(path, Json{{"manifest", file.manifest}, {"executable", file.executable}});
      }
      const auto revision = Json{{"version", 1},
                                 {"parents", Json::array({plan.head, std::string(source)})},
                                 {"message", message},
                                 {"timestamp", unix_seconds()},
                                 {"files", files}};
      throw_if_cancelled(options.stop);
      target = put(Kind::revision, bytes(revision.dump())).first.id;
    }
    result = checkout_directory(directory, branch, target, options);
  }
  result["operation"] = "merge";
  result["mode"] = mode;
  result["previous_revision"] = plan.head;
  result["merged_revision"] = source;
  result["base"] = plan.base ? Json(*plan.base) : Json{};
  return result;
}
} // namespace syrnck
