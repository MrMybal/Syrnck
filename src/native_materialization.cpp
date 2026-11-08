#include "native_materialization.hpp"
#include <algorithm>
namespace syrnck {
bool NativeMaterialization::covers(std::string path) const {
  for (;;) {
    if (folded_.contains(path)) return true;
    const auto slash = path.rfind('/');
    if (slash == path.npos) return false;
    path.resize(slash);
  }
}
NativeMaterialization NativeMaterialization::from_prefixes(std::vector<std::string> prefixes) {
  require(prefixes.size() <= max_prefixes, "native materialization exceeds 1024 prefixes");
  NativeMaterialization result;
  result.complete_ = false;
  std::size_t budget = 0;
  for (const auto &path : prefixes) {
    require(path.size() < max_prefix_bytes - budget, "native materialization prefixes exceed 64 KiB");
    budget += path.size() + 1;
    validate_path(path);
    auto folded = portable_fold(path);
    require(!result.covers(folded), "native materialization prefixes overlap");
    const auto descendant = result.folded_.lower_bound(folded + '/');
    require(descendant == result.folded_.end() || !descendant->starts_with(folded + '/'),
            "native materialization prefixes overlap");
    result.folded_.insert(std::move(folded));
  }
  std::sort(prefixes.begin(), prefixes.end());
  result.prefixes_ = std::move(prefixes);
  return result;
}
NativeMaterialization NativeMaterialization::from_definition(const Json &value) {
  if (value.is_null()) return {};
  require(value.is_array() && value.size() <= max_prefixes, "invalid native materialization");
  std::vector<std::string> prefixes;
  for (const auto &path : value) {
    require(path.is_string(), "invalid native materialization");
    prefixes.push_back(path.get<std::string>());
  }
  const auto result = from_prefixes(std::move(prefixes));
  require(result.definition() == value, "invalid native materialization");
  return result;
}
NativeMaterialization NativeMaterialization::decode(std::span<const std::uint8_t> data) {
  require(data.size() <= 96 * 1024, "invalid native materialization");
  const auto value = parse_json(data);
  require(value.is_object() && value.size() == 3 && value.contains("version") &&
              value["version"].is_number_unsigned() && value["version"] == 1 &&
              value.contains("type") && value["type"].is_string() &&
              value["type"] == "native-materialization" && value.contains("prefixes") &&
              value["prefixes"].is_array(), "invalid native materialization");
  const auto result = from_definition(value["prefixes"]);
  require(result.encode() == text(data), "invalid native materialization");
  return result;
}
bool NativeMaterialization::includes(std::string_view path) const {
  return complete_ || covers(portable_fold(path));
}
bool NativeMaterialization::intersects(std::string_view path) const {
  if (complete_ || path.empty()) return true;
  const auto folded = portable_fold(path);
  if (covers(folded)) return true;
  const auto prefix = folded + '/';
  const auto descendant = folded_.lower_bound(prefix);
  return descendant != folded_.end() && descendant->starts_with(prefix);
}
Json NativeMaterialization::definition() const { return complete_ ? Json{} : Json(prefixes_); }
std::string NativeMaterialization::encode() const {
  require(!complete_, "complete native materialization has no descriptor");
  return Json{{"version", 1}, {"type", "native-materialization"}, {"prefixes", prefixes_}}.dump();
}
std::string NativeMaterialization::identity() const {
  return complete_ ? std::string("all") : object_id(Kind::chunk, bytes(encode()));
}
} // namespace syrnck
