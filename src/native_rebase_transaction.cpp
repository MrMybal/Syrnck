#include "native_rebase.hpp"
namespace syrnck {
Json Store::rebase_directory(const fs::path &directory, std::string_view branch, std::string_view expected,
                             std::string_view base, std::string_view onto,
                             const NativeRebaseOptions &settings, OperationOptions options) const {
  throw_if_cancelled(options.stop);
  validate_ref("heads/" + std::string(branch));
  validate_id(expected);
  require_no_native_checkout(*this);
  const auto all = refs();
  const auto found = all.find("heads/" + std::string(branch));
  require(found != all.end() && found->second.kind == Kind::revision, "unknown native branch");
  require(found->second.id == expected, "native rebase tip changed; inspect the branch and retry");
  require(!all.contains("staging/" + std::string(branch)), "native rebase requires an empty staging index");
  auto plan = prepare_native_rebase(*this, directory, branch, base, onto, settings, true, options);
  require(plan.report["ready"].get<bool>(), "native rebase is blocked; inspect native rebase-plan");
  const auto mode = plan.report["mode"].get<std::string>();
  const auto target = plan.target;
  const auto count = plan.report["commits"].size();
  plan.files.clear();
  plan.report = Json{};
  Json result;
  if (mode == "up_to_date") {
    result = Json{{"version", 1}, {"pending", false}, {"state", "unchanged"}, {"target", target}};
  } else {
    if (mode == "rebase") {
      auto prepared =
          prepare_native_rebase(*this, directory, branch, base, onto, settings, false, options,
                                [&](const Json &revision, const std::string &id) {
                                  throw_if_cancelled(options.stop);
                                  const auto key = put(Kind::revision, bytes(revision.dump())).first;
                                  require(key.id == id, "native rebase candidate changed; refresh the plan");
                                });
      require(prepared.report["ready"].get<bool>() && prepared.target == target,
              "native rebase candidate changed; refresh the plan");
    }
    result = checkout_directory(directory, branch, target, options);
  }
  result["operation"] = "rebase";
  result["mode"] = mode;
  result["previous_revision"] = expected;
  result["base"] = base;
  result["onto"] = onto;
  result["replayed_commits"] = mode == "rebase" ? count : 0;
  return result;
}
} // namespace syrnck
