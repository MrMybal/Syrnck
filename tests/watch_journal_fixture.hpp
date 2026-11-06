#pragma once
#include "internal.hpp"
namespace watch_fixture {
using namespace syrnck;
inline Json record(bool complete = false) {
  return Json{{"version", 1}, {"type", "native-watch-journal"}, {"workspace", std::string(64, '1')},
              {"session", std::string(64, '2')}, {"generation", std::uint64_t(complete ? 2 : 1)},
              {"selection", std::string(64, '3')}, {"state", complete ? "observed" : "pending"},
              {"backend", "polling"}, {"previous", Json{{"state", "missing"}, {"session", Json{}}}},
              {"batch", Json{{"full_scan", true}, {"overflow", false}, {"polling", false},
                             {"paths", Json::array({"private-asset-name.uasset"})}}},
              {"observation", complete ? Json{{"digest", std::string(64, '4')}, {"base", Json{}},
                                              {"clean", true}, {"workspace_files", std::uint64_t(1)}} : Json{}}};
}
inline std::optional<Key> key(bool encrypted) {
  if (!encrypted) return {};
  Key value{}; value.fill(39); return value;
}
} // namespace watch_fixture
