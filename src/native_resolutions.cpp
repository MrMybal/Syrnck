#include "internal.hpp"
namespace syrnck {
namespace {
std::string_view choice_name(NativeResolutionChoice choice) {
  switch (choice) {
  case NativeResolutionChoice::current:
    return "current";
  case NativeResolutionChoice::incoming:
    return "incoming";
  case NativeResolutionChoice::base:
    return "base";
  case NativeResolutionChoice::remove:
    return "remove";
  case NativeResolutionChoice::file:
    return "file";
  }
  throw Error("invalid native resolution choice");
}
Json entry_json(const NativeFileResolution &entry) {
  Json row{{"choice", choice_name(entry.choice)}};
  if (entry.choice == NativeResolutionChoice::file) {
    row["manifest"] = entry.manifest;
    row["executable"] = entry.executable;
  }
  return row;
}
NativeResolutionSet from_tree(const nlohmann::json &value) {
  require(value.is_object() && value.size() == 7 && value.contains("version") &&
              value["version"].is_number_integer() && value["version"] == 1 && value.contains("operation") &&
              value["operation"].is_string() && value.contains("head") && value["head"].is_string() &&
              value.contains("source") && value["source"].is_string() && value.contains("base") &&
              (value["base"].is_null() || value["base"].is_string()) && value.contains("parent") &&
              (value["parent"].is_null() || value["parent"].is_string()) && value.contains("files") &&
              value["files"].is_object() && value["files"].size() <= 100000,
          "invalid native resolution document");
  NativeResolutionSet result{value["operation"].get<std::string>(),
                             value["head"].get<std::string>(),
                             value["source"].get<std::string>(),
                             {},
                             {},
                             {}};
  if (!value["base"].is_null())
    result.base = value["base"].get<std::string>();
  if (!value["parent"].is_null())
    result.parent = value["parent"].get<std::string>();
  for (const auto &[path, row] : value["files"].items()) {
    require(row.is_object() && row.contains("choice") && row["choice"].is_string(),
            "invalid native resolution document");
    const auto name = row["choice"].get<std::string>();
    NativeFileResolution entry;
    if (name == "current")
      entry.choice = NativeResolutionChoice::current;
    else if (name == "incoming")
      entry.choice = NativeResolutionChoice::incoming;
    else if (name == "base")
      entry.choice = NativeResolutionChoice::base;
    else if (name == "remove")
      entry.choice = NativeResolutionChoice::remove;
    else if (name == "file")
      entry.choice = NativeResolutionChoice::file;
    else
      throw Error("invalid native resolution choice");
    if (entry.choice == NativeResolutionChoice::file) {
      require(row.size() == 3 && row.contains("manifest") && row["manifest"].is_string() &&
                  row.contains("executable") && row["executable"].is_boolean(),
              "invalid native resolution document");
      entry.manifest = row["manifest"].get<std::string>();
      entry.executable = row["executable"].get<bool>();
    } else {
      require(row.size() == 1, "invalid native resolution document");
    }
    result.files.emplace_hint(result.files.end(), path, std::move(entry));
  }
  result.validate();
  return result;
}
class Keys final : public nlohmann::json_sax<nlohmann::json> {
  std::vector<std::optional<std::set<std::string>>> stack_;
  std::size_t fields_ = 0;
  bool start(bool object) {
    require(stack_.size() < 64, "invalid native resolution document");
    stack_.push_back(object ? std::optional<std::set<std::string>>{std::in_place} : std::nullopt);
    return true;
  }

public:
  bool null() override {
    return true;
  }
  bool boolean(bool) override {
    return true;
  }
  bool number_integer(number_integer_t) override {
    return true;
  }
  bool number_unsigned(number_unsigned_t) override {
    return true;
  }
  bool number_float(number_float_t, const string_t &) override {
    return true;
  }
  bool string(string_t &) override {
    return true;
  }
  bool binary(binary_t &) override {
    return true;
  }
  bool start_object(std::size_t) override {
    return start(true);
  }
  bool key(string_t &key) override {
    require(++fields_ <= 400010, "native resolution field limit exceeded");
    require(!stack_.empty() && stack_.back() && stack_.back()->insert(key).second,
            "duplicate native resolution field");
    return true;
  }
  bool end_object() override {
    stack_.pop_back();
    return true;
  }
  bool start_array(std::size_t) override {
    return start(false);
  }
  bool end_array() override {
    stack_.pop_back();
    return true;
  }
  bool parse_error(std::size_t, const std::string &, const nlohmann::detail::exception &) override {
    return false;
  }
};
} // namespace
void NativeResolutionSet::validate() const {
  require(operation == "merge" || operation == "cherry-pick" || operation == "revert",
          "invalid native resolution operation");
  validate_id(head);
  validate_id(source);
  if (base)
    validate_id(*base);
  if (parent)
    validate_id(*parent);
  require(files.size() <= 100000, "native resolution path limit exceeded");
  std::size_t size = 512;
  for (const auto &[path, entry] : files) {
    validate_path(path);
    (void)choice_name(entry.choice);
    if (entry.choice == NativeResolutionChoice::file)
      validate_id(entry.manifest);
    else
      require(entry.manifest.empty() && !entry.executable, "invalid native resolution document");
    const auto bytes = Json(path).dump().size() + entry_json(entry).dump().size() + 2;
    require(bytes <= max_object_size - size, "native resolution document exceeds 16 MiB");
    size += bytes;
  }
}
Json NativeResolutionSet::json() const {
  validate();
  Json entries = Json::object();
  auto &object = entries.get_ref<Json::object_t &>();
  object.reserve(files.size());
  for (const auto &[path, entry] : files)
    object.emplace_back(path, entry_json(entry));
  return Json{{"version", 1},
              {"operation", operation},
              {"head", head},
              {"source", source},
              {"base", base ? Json(*base) : Json{}},
              {"parent", parent ? Json(*parent) : Json{}},
              {"files", entries}};
}
NativeResolutionSet NativeResolutionSet::parse(const Json &value) {
  return from_tree(nlohmann::json(value));
}
NativeResolutionSet NativeResolutionSet::decode(std::span<const std::uint8_t> data) {
  require(data.size() <= max_object_size, "native resolution document exceeds 16 MiB");
  Keys guard;
  require(nlohmann::json::sax_parse(data.begin(), data.end(), &guard), "invalid native resolution document");
  return from_tree(nlohmann::json::parse(data.begin(), data.end()));
}
} // namespace syrnck
