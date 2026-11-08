#include "native_materialization.hpp"
#include "native_materialization_transition.hpp"
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
  if (complete_) return true;
  const auto folded = portable_fold(path);
  const auto slash = folded.rfind('/');
  const auto leaf = slash == folded.npos ? folded : folded.substr(slash + 1);
  if (leaf == ".syrnckignore" && !prefixes_.empty() &&
      intersects(slash == folded.npos ? std::string_view{} : std::string_view(folded).substr(0, slash))) {
    const auto spelling = path.substr(path.rfind('/') == path.npos ? 0 : path.rfind('/') + 1);
    require(spelling == ".syrnckignore", "native ignore control must use canonical spelling");
    return true;
  }
  return covers(folded);
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
bool native_materialization_reference(std::string_view name, std::string *context) {
  if (name == "workspace/materialization") {
    if (context) context->clear();
    return true;
  }
  if (!name.starts_with("workspaces/")) return false;
  const auto slash = name.find('/', 11);
  if (slash == name.npos || name.substr(slash + 1) != "workspace/materialization") return false;
  const auto selected = name.substr(11, slash - 11);
  validate_native_workspace_name(selected);
  if (context) *context = selected;
  return true;
}
namespace {
bool sparse_required(const Store &store) {
  const auto features = store.required_features();
  return std::find(features.begin(), features.end(), "sparse-workspaces-v1") != features.end();
}
bool workspace_active(const Refs &refs, std::string_view context) {
  if (!context.empty()) return refs.contains(native_context_reference(context, "registration"));
  const auto heads = refs.lower_bound("heads/");
  const auto staging = refs.lower_bound("staging/");
  return refs.contains("workspace/HEAD") || refs.contains("workspace/detached") ||
      refs.contains("workspace/staging") || refs.contains("checkouts/active") ||
      (heads != refs.end() && heads->first.starts_with("heads/")) ||
      (staging != refs.end() && staging->first.starts_with("staging/"));
}
std::optional<ObjectKey> lookup(const Refs &refs, const std::string &name) {
  const auto found = refs.find(name);
  return found == refs.end() ? std::optional<ObjectKey>{} : std::optional{found->second};
}
}
NativeMaterializationState read_native_materialization(const Store &store, std::string_view context,
                                                        const Refs &refs, bool require_feature) {
  NativeMaterializationState state;
  state.reference = native_context_reference(context, "workspace/materialization");
  state.root = lookup(refs, state.reference);
  if (!state.root) return state;
  require(!require_feature || sparse_required(store), "workspace materialization requires a supported reader feature");
  require(state.root->kind == Kind::chunk, "invalid native materialization");
  state.policy = NativeMaterialization::decode(store.get(*state.root));
  return state;
}
NativeMaterializationState read_native_materialization(const Store &store) {
  if (!sparse_required(store))
    return {{}, {}, store.native_workspace_reference("workspace/materialization")};
  return read_native_materialization(store, store.native_workspace_name(), store.refs());
}
void validate_native_materialization_requirements(const Store &store, const Refs &refs) {
  for (const auto &[name, key] : refs) {
    (void)key;
    if (native_materialization_reference(name))
      require(sparse_required(store), "workspace materialization requires a supported reader feature");
  }
}
bool validate_native_materialization_changes(const Store &store, const Refs &before, const Refs &after) {
  bool required = false;
  std::set<std::string> policies;
  for (const auto *refs : {&before, &after})
    for (const auto &[name, key] : *refs) {
      (void)key;
      if (native_materialization_reference(name)) policies.insert(name);
    }
  for (const auto &name : policies) {
    std::string context;
    require(native_materialization_reference(name, &context), "invalid native materialization");
    const auto previous = lookup(before, name), next = lookup(after, name);
    const bool was_active = workspace_active(before, context), is_active = workspace_active(after, context);
    require(previous == next || !was_active || !is_active ||
                validate_native_materialization_transition(store, context, before, after),
            "live workspace materialization is immutable");
    if (next) {
      // Immutable object IDs already validated at publication need no payload
      // reread on every unrelated journal/index update. Revalidate a newly
      // installed policy or a portable policy becoming an active context.
      if (previous != next || (!was_active && is_active))
        (void)read_native_materialization(store, context, after, false);
      required = true;
    }
  }
  return required;
}
void require_complete_native_materialization(const Store &store) {
  require(read_native_materialization(store).policy.complete(),
          "operation requires complete workspace materialization");
}
} // namespace syrnck
