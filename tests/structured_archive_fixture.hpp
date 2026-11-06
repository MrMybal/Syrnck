#pragma once
#include "native_history_fixture.hpp"
#include <syrnck/backup.hpp>
#include <syrnck/retention.hpp>
namespace structured_archive_test {
using namespace native_test;
inline std::optional<Key> encryption_key(bool encrypted, std::uint8_t value) {
  if (!encrypted)
    return {};
  Key key;
  key.fill(value);
  return key;
}
struct Archives {
  std::string parent, tip, current, index, branch, plain_branch, tag, light;
  std::vector<std::string> selectors;
  Json json() const {
    return Json{{"parent", parent}, {"tip", tip},       {"current", current},
                {"index", index},   {"branch", branch}, {"plain_branch", plain_branch},
                {"tag", tag},       {"light", light},   {"selectors", selectors}};
  }
};
inline Archives populate(Fixture &f) {
  Archives a;
  a.parent = f.revision(Json{{"asset", f.file("original")}});
  a.tip = f.revision(Json{{"asset", f.file("old branch tip")}}, Json::array({a.parent}));
  a.current = f.revision(Json{{"asset", f.file("current unrelated")}});
  f.install(a.current);
  f.store.create_native_branch("feature", a.parent);
  f.write("asset", "prepared archived index");
  a.index = f.store.stage_directory(f.project, "feature", {"asset"});
  f.store.set_ref("heads/feature", {Kind::revision, a.tip});
  a.branch = f.store.delete_native_branch("feature", a.tip, a.index)["archive"].get<std::string>();
  f.store.create_native_branch("plain", a.tip);
  a.plain_branch = f.store.delete_native_branch("plain", a.tip)["archive"].get<std::string>();
  f.store.create_native_tag("release/v1", a.tip, "Retained notes: \xc3\xa9");
  a.tag = f.store.delete_native_tag("release/v1", a.tip)["archive"].get<std::string>();
  f.store.create_native_tag("light", a.parent);
  a.light = f.store.delete_native_tag("light", a.parent)["archive"].get<std::string>();
  const auto simple = "archive/amend/" + a.parent;
  f.store.set_ref(simple, {Kind::revision, a.parent});
  a.selectors = {"archive/branches/" + a.branch, "archive/branches/" + a.plain_branch,
                 "archive/tags/" + a.tag, "archive/tags/" + a.light, simple};
  f.write("asset", "dirty current");
  f.write("untracked", "leave alone");
  return a;
}
inline void reuse_names(const Store &store, const Archives &a) {
  store.create_native_branch("feature", a.current);
  store.create_native_branch("plain", a.current);
  store.create_native_tag("release/v1", a.current, "New release");
  store.create_native_tag("light", a.current);
}
inline Refs retired_refs(Refs refs, const std::vector<std::string> &selectors) {
  for (const auto &selector : selectors) {
    refs.erase(selector);
    const auto prefix = selector + '/';
    for (auto it = refs.lower_bound(prefix); it != refs.end() && it->first.starts_with(prefix);)
      it = refs.erase(it);
  }
  return refs;
}
inline void check_bytes(const Store &store, const std::string &revision, std::string_view expected) {
  const auto value = store.metadata(Kind::revision, revision);
  std::ostringstream output;
  store.reconstruct(value["files"]["asset"]["manifest"].get<std::string>(), output);
  require(output.str() == expected, "structured retirement lost retained file bytes");
}
} // namespace structured_archive_test
