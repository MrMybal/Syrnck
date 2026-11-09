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
  std::size_t maximum_;
  bool start(bool object) {
    require(stack_.size() < 64, "invalid native resolution document");
    stack_.push_back(object ? std::optional<std::set<std::string>>{std::in_place} : std::nullopt);
    return true;
  }

public:
  explicit Keys(std::size_t maximum = 400010) : maximum_(maximum) {}
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
    require(++fields_ <= maximum_, "native resolution field limit exceeded");
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
NativeRebaseResolutionSet rebase_from_tree(const nlohmann::json &value) {
  require(value.is_object() && value.size() == 5 && value.contains("version") &&
              value["version"].is_number_integer() && value["version"] == 1 && value.contains("head") &&
              value["head"].is_string() && value.contains("base") && value["base"].is_string() &&
              value.contains("onto") && value["onto"].is_string() && value.contains("steps") &&
              value["steps"].is_array() && value["steps"].size() <= 100000,
          "invalid native rebase resolution document");
  NativeRebaseResolutionSet result{value["head"].get<std::string>(),
                                   value["base"].get<std::string>(),
                                   value["onto"].get<std::string>(),
                                   {}};
  for (const auto &row : value["steps"]) {
    auto step = from_tree(row);
    const auto source = step.source;
    require(result.steps.emplace(source, std::move(step)).second, "duplicate native rebase resolution step");
  }
  result.validate();
  return result;
}
NativeRebaseTodo todo_from_tree(const nlohmann::json &value) {
  require(value.is_object() && value.size() == 6 && value.contains("version") &&
              value["version"].is_number_integer() && value["version"] == 1 && value.contains("type") &&
              value["type"] == "native-rebase-todo" && value.contains("head") && value["head"].is_string() &&
              value.contains("base") && value["base"].is_string() && value.contains("onto") &&
              value["onto"].is_string() && value.contains("steps") && value["steps"].is_array() &&
              value["steps"].size() <= 100000,
          "invalid native rebase todo document");
  NativeRebaseTodo result{value["head"].get<std::string>(),
                          value["base"].get<std::string>(),
                          value["onto"].get<std::string>(),
                          {}};
  for (const auto &row : value["steps"]) {
    require(row.is_object() && row.contains("source") && row["source"].is_string() &&
                row.contains("action") && row["action"].is_string() &&
                ((row.size() == 2 && !row.contains("message")) ||
                 (row.size() == 3 && row.contains("message") && row["message"].is_string())),
            "invalid native rebase todo document");
    NativeRebaseTodoStep step{row["source"].get<std::string>(), row["action"].get<std::string>(), {}};
    if (row.contains("message"))
      step.message = row["message"].get<std::string>();
    result.steps.push_back(std::move(step));
  }
  result.validate();
  return result;
}

} // namespace
void NativeResolutionSet::validate() const {
  require(operation == "merge" || operation == "cherry-pick" || operation == "revert" ||
              operation == "rebase" || operation == "rebase-index" || operation == "stash" ||
              operation == "stash-index",
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
void NativeRebaseResolutionSet::validate() const {
  validate_id(head);
  validate_id(base);
  validate_id(onto);
  require(steps.size() <= 100000, "native rebase resolution step limit exceeded");
  std::size_t total = 512, paths = 0;
  for (const auto &[source, step] : steps) {
    require(source == step.source && step.operation == "rebase", "invalid native rebase resolution document");
    step.validate();
    require(step.files.size() <= 100000 - paths, "native resolution path limit exceeded");
    paths += step.files.size();
    const auto size = step.json().dump().size() + 1;
    require(size <= max_object_size - total, "native resolution document exceeds 16 MiB");
    total += size;
  }
}
Json NativeRebaseResolutionSet::json() const {
  validate();
  Json entries = Json::array();
  for (const auto &[source, step] : steps) {
    (void)source;
    entries.push_back(step.json());
  }
  return Json{{"version", 1}, {"head", head}, {"base", base}, {"onto", onto}, {"steps", std::move(entries)}};
}
NativeRebaseResolutionSet NativeRebaseResolutionSet::parse(const Json &value) {
  return rebase_from_tree(nlohmann::json(value));
}
NativeRebaseResolutionSet NativeRebaseResolutionSet::decode(std::span<const std::uint8_t> data) {
  require(data.size() <= max_object_size, "native resolution document exceeds 16 MiB");
  Keys guard(1100010);
  require(nlohmann::json::sax_parse(data.begin(), data.end(), &guard),
          "invalid native rebase resolution document");
  return rebase_from_tree(nlohmann::json::parse(data.begin(), data.end()));
}
void NativeRebaseTodo::validate() const {
  validate_id(head);
  validate_id(base);
  validate_id(onto);
  require(steps.size() <= 100000, "native rebase todo step limit exceeded");
  std::set<std::string> seen;
  bool previous = false;
  std::size_t total = 512;
  for (const auto &step : steps) {
    validate_id(step.source);
    require(seen.insert(step.source).second, "duplicate native rebase todo source");
    require(step.action == "pick" || step.action == "drop" || step.action == "reword" ||
                step.action == "squash" || step.action == "fixup" || step.action == "merge",
            "invalid native rebase todo action");
    require(step.message.has_value() == (step.action == "reword"), "invalid native rebase todo message");
    if (step.action == "squash" || step.action == "fixup")
      require(previous, "native rebase fold requires a preceding picked commit");
    if (step.action != "drop")
      previous = true;
    Json row{{"source", step.source}, {"action", step.action}};
    if (step.message) {
      require(step.message->size() <= 1024 * 1024, "native rebase message exceeds 1 MiB");
      row["message"] = *step.message;
    }
    std::size_t cost;
    try {
      cost = row.dump().size() + 1;
    } catch (const nlohmann::json::exception &) {
      throw Error("invalid native rebase todo message");
    }
    require(cost <= max_object_size - total, "native rebase todo document exceeds 16 MiB");
    total += cost;
  }
}
Json NativeRebaseTodo::json() const {
  validate();
  Json entries = Json::array();
  for (const auto &step : steps) {
    Json row{{"source", step.source}, {"action", step.action}};
    if (step.message)
      row["message"] = *step.message;
    entries.push_back(std::move(row));
  }
  return Json{{"version", 1}, {"type", "native-rebase-todo"}, {"head", head}, {"base", base},
              {"onto", onto}, {"steps", std::move(entries)}};
}
NativeRebaseTodo NativeRebaseTodo::parse(const Json &value) {
  return todo_from_tree(nlohmann::json(value));
}
NativeRebaseTodo NativeRebaseTodo::decode(std::span<const std::uint8_t> data) {
  require(data.size() <= max_object_size, "native rebase todo document exceeds 16 MiB");
  Keys guard(300010);
  require(nlohmann::json::sax_parse(data.begin(), data.end(), &guard), "invalid native rebase todo document");
  return todo_from_tree(nlohmann::json::parse(data.begin(), data.end()));
}

} // namespace syrnck
