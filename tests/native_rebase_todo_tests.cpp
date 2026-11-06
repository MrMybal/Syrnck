#include "native_history_fixture.hpp"
#include <iostream>
using namespace native_test;
namespace {
void schema() {
  const auto a = std::string(64, 'a'), b = std::string(64, 'b'), c = std::string(64, 'c');
  NativeRebaseTodo todo{c, a, b, {{a, "reword", "a new message"}, {b, "fixup", {}}}};
  const auto value = todo.json();
  require(NativeRebaseTodo::parse(value).json() == value &&
              NativeRebaseTodo::decode(bytes(value.dump())).json() == value,
          "todo schema changed on roundtrip");
  for (const auto &patch : {Json{{"version", 2}}, Json{{"type", "other"}}, Json{{"extra", true}},
                            Json{{"head", "HEAD"}}, Json{{"steps", Json::object()}}}) {
    auto invalid = value;
    invalid.update(patch);
    fails([&] { NativeRebaseTodo::parse(invalid); });
  }
  for (const auto &row : {Json{{"source", a}, {"action", "exec"}}, Json{{"source", a}, {"action", "reword"}},
                          Json{{"source", a}, {"action", "pick"}, {"message", "unexpected"}},
                          Json{{"source", a}, {"action", "pick"}, {"unknown", false}},
                          Json{{"source", a}, {"action", "squash"}}}) {
    auto invalid = value;
    invalid["steps"] = Json::array({row});
    fails([&] { NativeRebaseTodo::parse(invalid); });
  }
  auto duplicate = value;
  duplicate["steps"].push_back(duplicate["steps"][0]);
  fails([&] { NativeRebaseTodo::parse(duplicate); });
  auto raw = value.dump();
  raw.insert(raw.size() - 1, ",\"head\":\"" + c + '"');
  fails([&] { NativeRebaseTodo::decode(bytes(raw)); });
  raw = value.dump();
  const auto position = raw.find("\"action\":\"fixup\"");
  raw.insert(position, "\"action\":\"drop\",");
  fails([&] { NativeRebaseTodo::decode(bytes(raw)); });
  fails([&] { NativeRebaseTodo::decode(bytes(std::string(65, '[') + "0" + std::string(65, ']'))); });
  fails([&] { NativeRebaseTodo::decode(bytes(std::string(max_object_size + 1, 'x'))); });
  todo.steps[0].message = std::string(1024 * 1024 + 1, 'x');
  fails([&] { todo.validate(); });
  todo.steps[0].message = std::string(1, static_cast<char>(0xff));
  fails([&] { todo.validate(); });
}
void workflow(const fs::path &path, std::optional<Key> key, unsigned layout) {
  Fixture f(path, key);
  const auto base_file = f.file("base"), a = f.file("A"), b = f.file("B"), c = f.file("C"), d = f.file("D"),
             upstream = f.file("upstream");
  const auto base = f.revision(Json{{"base", base_file}});
  const auto first = f.revision(Json{{"base", base_file}, {"a", a}}, Json::array({base}));
  auto second = f.revision(Json{{"base", base_file}, {"a", a}, {"b", b}}, Json::array({first}));
  auto second_metadata = f.store.metadata(Kind::revision, second);
  second_metadata["timestamp"] = 29;
  second = f.store.put(Kind::revision, bytes(second_metadata.dump())).first.id;
  const auto third =
      f.revision(Json{{"base", base_file}, {"a", a}, {"b", b}, {"c", c}}, Json::array({second}));
  const auto head =
      f.revision(Json{{"base", base_file}, {"a", a}, {"b", b}, {"c", c}, {"d", d}}, Json::array({third}));
  const auto onto = f.revision(Json{{"base", base_file}, {"upstream", upstream}}, Json::array({base}));
  f.install(head);
  f.pack(layout);
  const auto before = f.physical();
  NativeRebaseOptions options;
  options.todo = NativeRebaseTodo{head,
                                  base,
                                  onto,
                                  {{second, "reword", "renamed second"},
                                   {first, "squash", {}},
                                   {third, "fixup", {}},
                                   {head, "drop", {}}}};
  const auto plan = f.store.plan_rebase(path, "main", base, onto, options, true);
  require(plan["ready"] == true && plan["completed_commits"] == 3 && plan["result_commits"] == 1 &&
              plan["dropped_commits"] == 1 && plan["steps"].size() == 4 && plan["result_files"] == 5 &&
              f.physical() == before,
          "edited rebase lost independent reordered deltas or wrote a plan");
  require(plan["steps"][0]["source"] == second && plan["steps"][1]["source"] == first &&
              plan["steps"][1]["replaces"] == plan["steps"][0]["target"] &&
              plan["steps"][2]["replaces"] == plan["steps"][1]["target"],
          "rebase fold report lost replacement identity");
  const auto target = plan["target"].get<std::string>();
  require(f.store.plan_rebase(path, "main", base, onto, options)["target"] == target,
          "edited rebase IDs depend on verification");
  auto invalid = options;
  invalid.todo->head = first;
  fails([&] { f.store.plan_rebase(path, "main", base, onto, invalid); });
  invalid = options;
  invalid.todo->steps.pop_back();
  fails([&] { f.store.plan_rebase(path, "main", base, onto, invalid); });
  invalid = options;
  invalid.todo->steps.back().source = onto;
  fails([&] { f.store.plan_rebase(path, "main", base, onto, invalid); });
  invalid = options;
  invalid.resolutions = NativeRebaseResolutionSet{head, base, onto, {}};
  invalid.resolutions->steps[head] = {"rebase", onto, head, third, third, {}};
  fails([&] { f.store.plan_rebase(path, "main", base, onto, invalid); });
  require(f.physical() == before, "invalid sequence wrote objects");
  const auto applied = f.store.rebase_directory(path, "main", head, base, onto, options);
  require(applied["target"] == target && applied["replayed_commits"] == 3 && applied["result_commits"] == 1 &&
              applied["dropped_commits"] == 1 && !fs::exists(path / "d") &&
              text(read_bytes(path / "a")) == "A" && text(read_bytes(path / "b")) == "B" &&
              text(read_bytes(path / "c")) == "C" && text(read_bytes(path / "upstream")) == "upstream",
          "edited rebase applied another snapshot");
  const auto next = f.store.metadata(Kind::revision, target);
  require(next["parents"] == Json::array({onto}) && next["timestamp"] == 29 &&
              next["message"] == "renamed second\n\n" +
                                     f.store.metadata(Kind::revision, first)["message"].get<std::string>(),
          "fold lost the preceding candidate metadata, message or parent");
  for (std::size_t i = 0; i < 2; ++i)
    require(!f.store.contains({Kind::revision, plan["steps"][i]["target"].get<std::string>()}),
            "folded provisional candidate was unnecessarily materialized");
  f.store.gc(true);
  for (const auto &source : {base, first, second, third, head})
    f.store.export_revision(source, f.store.root() / source);
  require(text(read_bytes(f.store.root() / head / "d")) == "D", "drop discarded archived historical blocks");
  require(f.store.fsck() > 0, "edited rebase damaged Store");
}
void dropped_dependencies(const fs::path &path) {
  Fixture f(path);
  const auto a = f.file("a"), b = f.file("b"), c = f.file("c"), extra = f.file("extra");
  const auto base = f.revision(Json{{"asset", a}});
  const auto first = f.revision(Json{{"asset", b}}, Json::array({base}));
  const auto head = f.revision(Json{{"asset", c}}, Json::array({first}));
  const auto onto = f.revision(Json{{"asset", a}, {"extra", extra}}, Json::array({base}));
  f.install(head);
  NativeRebaseOptions options;
  options.todo = NativeRebaseTodo{head, base, onto, {{first, "drop", {}}, {head, "pick", {}}}};
  const auto before = f.physical();
  const auto blocked = f.store.plan_rebase(path, "main", base, onto, options);
  require(blocked["ready"] == false && blocked["steps"][1]["conflicts"].size() == 1 && f.physical() == before,
          "dropping a dependency silently lost its conflict");
  options.resolutions = NativeRebaseResolutionSet::parse(blocked["resolution_context"]);
  auto choice = NativeResolutionSet::parse(blocked["steps"][1]["resolution_context"]);
  choice.files["asset"] = {NativeResolutionChoice::incoming};
  options.resolutions->steps[head] = choice;
  const auto ready = f.store.plan_rebase(path, "main", base, onto, options, true);
  require(ready["ready"] == true && f.physical() == before, "explicit dropped-dependency resolution failed");
  f.store.rebase_directory(path, "main", head, base, onto, options);
  require(text(read_bytes(path / "asset")) == "c", "resolved rebase chose another side");
}
void same_base_and_all_dropped(const fs::path &path) {
  Fixture f(path);
  const auto a = f.file("a"), b = f.file("b");
  const auto base = f.revision(Json{{"a", a}});
  const auto head = f.revision(Json{{"a", a}, {"b", b}}, Json::array({base}));
  f.install(head);
  NativeRebaseOptions options;
  options.todo = NativeRebaseTodo{head, base, base, {{head, "drop", {}}}};
  const auto plan = f.store.plan_rebase(path, "main", base, base, options);
  require(plan["ready"] == true && plan["mode"] == "rebase" && plan["target"] == base &&
              plan["result_commits"] == 0,
          "same-base edited sequence was skipped");
  f.store.rebase_directory(path, "main", head, base, base, options);
  require(f.store.refs().at("heads/main").id == base && !fs::exists(path / "b"),
          "all-drop sequence did not move to onto");
  f.store.gc(true);
  f.store.export_revision(head, f.store.root() / "saved");
  require(text(read_bytes(f.store.root() / "saved/b")) == "b", "all-drop sequence lost its recovery archive");
}
} // namespace
int main() {
  Temporary temp;
  try {
    schema();
    for (bool encrypted : {false, true})
      for (unsigned layout = 0; layout < 3; ++layout) {
        std::optional<Key> key;
        if (encrypted) {
          key.emplace();
          key->fill(73);
        }
        workflow(temp.path / (std::to_string(encrypted) + '-' + std::to_string(layout)), key, layout);
      }
    dropped_dependencies(temp.path / "dependencies");
    same_base_and_all_dropped(temp.path / "drop-all");
    std::cout
        << "Edited rebase ordering, folds, drop conflicts, strict documents and retained source history\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
