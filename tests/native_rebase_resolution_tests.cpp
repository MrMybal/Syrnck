#include "native_history_fixture.hpp"
#include <iostream>
using namespace native_test;
namespace {
void schema() {
  const auto id = std::string(64, 'a'), other = std::string(64, 'b');
  NativeRebaseResolutionSet set{id, id, other, {}};
  set.steps[id] =
      NativeResolutionSet{"rebase", other, id, id, id, {{"asset", {NativeResolutionChoice::current}}}};
  const auto value = set.json();
  require(NativeRebaseResolutionSet::parse(value).json() == value &&
              NativeRebaseResolutionSet::decode(bytes(value.dump())).json() == value,
          "rebase resolution schema roundtrip differs");
  for (const auto &bad : {Json{{"version", 2}}, Json{{"extra", true}}, Json{{"head", "HEAD"}},
                          Json{{"steps", Json::object()}}, Json{{"base", false}}}) {
    auto invalid = value;
    invalid.update(bad);
    fails([&] { NativeRebaseResolutionSet::parse(invalid); });
  }
  auto duplicate = value;
  duplicate["steps"].push_back(duplicate["steps"][0]);
  fails([&] { NativeRebaseResolutionSet::parse(duplicate); });
  auto raw = value.dump();
  raw.insert(raw.size() - 1, ",\"head\":\"" + id + "\"");
  fails([&] { NativeRebaseResolutionSet::decode(bytes(raw)); });
  fails([&] { NativeRebaseResolutionSet::decode(bytes(std::string(65, '[') + "0" + std::string(65, ']'))); });
  fails([&] { NativeRebaseResolutionSet::decode(bytes(std::string(max_object_size + 1, 'x'))); });
  set.steps[id].operation = "merge";
  fails([&] { set.validate(); });
}
void choices(const fs::path &path, std::optional<Key> key, unsigned physical) {
  Fixture f(path, key);
  const auto a = f.file("base a"), b = f.file("base b"), sa = f.file("source a"), sb = f.file("source b"),
             oa = f.file("onto a"), ob = f.file("onto b"), custom = f.file("manual a"),
             custom_b = f.file("manual b", true);
  const auto base = f.revision(Json{{"a", a}, {"b", b}});
  const auto first = f.revision(Json{{"a", sa}, {"b", b}}, Json::array({base}));
  const auto head = f.revision(Json{{"a", sa}, {"b", sb}}, Json::array({first}));
  const auto onto = f.revision(Json{{"a", oa}, {"b", ob}}, Json::array({base}));
  f.install(head);
  f.store.set_ref("heads/upstream", {Kind::revision, onto});
  f.store.set_ref("pins/custom-a", {Kind::manifest, custom["manifest"].get<std::string>()});
  f.store.set_ref("pins/custom-b", {Kind::manifest, custom_b["manifest"].get<std::string>()});
  f.pack(physical);
  const auto initial = f.store.plan_rebase(path, "main", base, onto);
  const auto context = NativeRebaseResolutionSet::parse(initial["resolution_context"]);
  const auto step_one = NativeResolutionSet::parse(initial["steps"][0]["resolution_context"]);
  for (const auto choice :
       {NativeResolutionChoice::current, NativeResolutionChoice::incoming, NativeResolutionChoice::base,
        NativeResolutionChoice::remove, NativeResolutionChoice::file}) {
    NativeRebaseOptions settings;
    settings.resolutions = context;
    settings.resolutions->steps[first] = step_one;
    auto &selected = settings.resolutions->steps[first];
    selected.files["a"] = {choice};
    if (choice == NativeResolutionChoice::file)
      selected.files["a"] = {choice, custom["manifest"].get<std::string>(), false};
    const auto before = f.physical();
    const auto partial = f.store.plan_rebase(path, "main", base, onto, settings);
    require(partial["ready"] == false && partial["completed_commits"] == 1 && partial["steps"].size() == 2 &&
                partial["steps"][1]["source"] == head && f.physical() == before,
            "rebase resolution hid a later conflict or wrote data");
    fails([&] { f.store.rebase_directory(path, "main", head, base, onto, settings); });
    require(f.physical() == before, "partial resolution stored candidates");
    auto step_two = NativeResolutionSet::parse(partial["steps"][1]["resolution_context"]);
    step_two.files["b"] = {NativeResolutionChoice::incoming};
    if (choice == NativeResolutionChoice::file)
      step_two.files["b"] = {NativeResolutionChoice::file, custom_b["manifest"].get<std::string>(), true};
    settings.resolutions->steps[head] = step_two;
    const auto ready = f.store.plan_rebase(path, "main", base, onto, settings, true);
    require(ready["ready"] == true && ready["steps"][0]["resolved"].size() == 1 &&
                ready["steps"][1]["resolved"].size() == 1 && f.physical() == before,
            "resolved rebase plan was not read-only or ready");
    auto stale = settings;
    stale.resolutions->head = onto;
    fails([&] { f.store.plan_rebase(path, "main", base, onto, stale); });
    stale = settings;
    stale.resolutions->steps[first].files["a"] = {choice == NativeResolutionChoice::incoming
                                                      ? NativeResolutionChoice::current
                                                      : NativeResolutionChoice::incoming};
    fails([&] { f.store.plan_rebase(path, "main", base, onto, stale); });
    stale = settings;
    stale.resolutions->steps[head].head = onto;
    fails([&] { f.store.rebase_directory(path, "main", head, base, onto, stale); });
    require(f.physical() == before, "stale rebase resolution wrote data");
    const auto applied = f.store.rebase_directory(path, "main", head, base, onto, settings);
    require(applied["target"] == ready["target"] && f.store.refs().at("heads/upstream").id == onto,
            "resolved rebase changed virtual IDs or upstream");
    const auto value = f.store.metadata(Kind::revision, applied["target"].get<std::string>());
    if (choice == NativeResolutionChoice::remove)
      require(!fs::exists(path / "a"), "resolved deletion was not applied");
    else {
      const auto expected = choice == NativeResolutionChoice::current    ? "onto a"
                            : choice == NativeResolutionChoice::incoming ? "source a"
                            : choice == NativeResolutionChoice::base     ? "base a"
                                                                         : "manual a";
      require(text(read_bytes(path / "a")) == expected, "resolved rebase chose wrong bytes");
    }
    require(text(read_bytes(path / "b")) ==
                (choice == NativeResolutionChoice::file ? "manual b" : "source b"),
            "second rebase resolution was lost");
    if (choice == NativeResolutionChoice::file)
      require(value["files"]["b"] == custom_b, "custom rebase manifest or mode changed");
    if (choice == NativeResolutionChoice::file) {
      f.store.remove_ref("pins/custom-a");
      f.store.remove_ref("pins/custom-b");
    }
    f.store.gc(true);
    (void)f.store.closure({{Kind::revision, head}, {Kind::revision, applied["target"].get<std::string>()}},
                          true);
    f.install(head);
  }
  NativeRebaseOptions invalid;
  invalid.resolutions = context;
  invalid.resolutions->steps[base] = NativeResolutionSet{"rebase", onto, base, base, base, {}};
  fails([&] { f.store.plan_rebase(path, "main", base, onto, invalid); });
  require(f.store.fsck() > 0, "resolved rebase damaged storage");
}
void missing_custom(const fs::path &path) {
  std::string base, head, onto, chunk;
  NativeRebaseOptions settings;
  {
    Fixture f(path);
    base = f.revision(Json{{"asset", f.file("base")}});
    head = f.revision(Json{{"asset", f.file("source")}}, Json::array({base}));
    onto = f.revision(Json{{"asset", f.file("onto")}}, Json::array({base}));
    f.install(head);
    const auto plan = f.store.plan_rebase(path, "main", base, onto);
    settings.resolutions = NativeRebaseResolutionSet::parse(plan["resolution_context"]);
    auto step = NativeResolutionSet::parse(plan["steps"][0]["resolution_context"]);
    const auto file = f.file("missing custom payload");
    const auto id = file["manifest"].get<std::string>();
    step.files["asset"] = {NativeResolutionChoice::file, id, false};
    settings.resolutions->steps[head] = step;
    chunk = f.store.metadata(Kind::manifest, id)["blocks"][0]["id"].get<std::string>();
  }
  require(fs::remove(path / ".syrnck/objects/chunk" / chunk.substr(0, 2) / chunk),
          "missing custom rebase fixture failed");
  Store store(path / ".syrnck");
  require(store.plan_rebase(path, "main", base, onto, settings)["ready"] == true,
          "metadata rebase plan read custom payload");
  const auto refs = store.refs();
  const auto objects = store.objects();
  fails([&] { store.plan_rebase(path, "main", base, onto, settings, true); });
  fails([&] { store.rebase_directory(path, "main", head, base, onto, settings); });
  require(store.refs() == refs && store.objects() == objects, "rebase accepted a missing custom block");
}
} // namespace
int main() {
  Temporary temp;
  try {
    schema();
    for (bool encrypted : {false, true})
      for (unsigned physical = 0; physical != 3; ++physical) {
        std::optional<Key> key;
        if (encrypted) {
          key.emplace();
          key->fill(51);
        }
        choices(temp.path / (std::to_string(encrypted) + '-' + std::to_string(physical)), key, physical);
      }
    missing_custom(temp.path / "missing");
    std::cout
        << "bound rebase resolution series, all choices, stale contexts, custom verification and GC passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
