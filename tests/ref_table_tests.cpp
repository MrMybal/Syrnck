#include "internal.hpp"
#include <iostream>
#include <sstream>
using namespace syrnck;
namespace {
template <class F> void fails(F function) {
  bool failed = false;
  try {
    function();
  } catch (const std::exception &) {
    failed = true;
  }
  require(failed, "expected reference table rejection");
}
Json previous_json(std::string_view data) {
  return Json::parse(data.begin(), data.end(), [](int depth, Json::parse_event_t, Json &) {
    require(depth <= 128, "JSON nesting exceeds 128 levels");
    return true;
  });
}
Refs previous_parser(std::string_view data) {
  auto document = previous_json(data);
  require(document.is_object(), "invalid refs document");
  Refs refs;
  for (const auto &[name, value] : document.items()) {
    validate_ref(name);
    refs.emplace(name, parse_key(value));
  }
  return refs;
}
} // namespace
int main() {
  auto root = fs::temp_directory_path() / ("syrnck-ref-tables-" + hex(random_bytes(16)));
  struct Cleanup {
    fs::path root;
    ~Cleanup() {
      std::error_code error;
      fs::remove_all(root, error);
    }
  } cleanup{root};
  try {
    auto same_json = [](std::string_view data) {
      std::optional<std::string> previous, current;
      try {
        previous = previous_json(data).dump();
      } catch (const std::exception &) {
      }
      try {
        current = parse_json(bytes(data)).dump();
      } catch (const std::exception &) {
      }
      require(previous == current, "JSON parser compatibility changed");
    };
    for (const auto &data : std::vector<std::string>{"null",
                                                     "true",
                                                     "false",
                                                     "-1",
                                                     "18446744073709551615",
                                                     "-9223372036854775808",
                                                     "1.25",
                                                     "-0.0",
                                                     "1e100",
                                                     "1e9999",
                                                     "NaN",
                                                     "[null,true,3,1.5,{},[]]",
                                                     "[1,]",
                                                     "{\"z\":0,\"a\":1}",
                                                     "{\"a\":{\"old\":1},\"a\":{\"new\":2}}",
                                                     "\"escaped\\t\\n\\u00e9\\ud83d\\ude80\"",
                                                     "\"\\ud800\"",
                                                     "\"\xff\"",
                                                     "{}{}",
                                                     "\xef\xbb\xbf{}",
                                                     "{\"truncated\":"})
      same_json(data);
    for (unsigned depth : {127, 128, 129, 130}) {
      same_json(std::string(depth, '[') + "0" + std::string(depth, ']'));
      same_json(std::string(depth, '[') + std::string(depth, ']'));
      same_json(std::string(depth, '[') + "{}" + std::string(depth, ']'));
      same_json(std::string(depth, '[') + "{\"key\":0}" + std::string(depth, ']'));
    }
    Json wide = Json::array();
    for (unsigned i = 0; i != 32768; ++i)
      wide.push_back(Json{{"index", i}});
    const auto wide_bytes = wide.dump();
    require(parse_json(bytes(wide_bytes)) == wide, "wide JSON array differs after parsing");
    const ObjectKey a{Kind::chunk, std::string(64, 'a')}, b{Kind::revision, std::string(64, 'b')};
    const auto first = key_json(a).dump(), last = key_json(b).dump();
    for (const auto &data : std::vector<std::string>{
             "{}", "{\"z\":" + first + ",\"a\":" + last + "}", "{\"a\":" + first + ",\"a\":" + last + "}",
             "{\"a\":null,\"\\u0061\":" + first + "}",
             "{\"escaped\\u002fref\":{\"id\":\"" + a.id + "\",\"kind\":\"chunk\"}}",
             "{\"a\":{\"kind\":\"wrong\",\"id\":\"" + a.id + "\",\"kind\":\"chunk\"}}"}) {
      const auto expected = previous_parser(data);
      const auto actual = parse_refs(bytes(data), "invalid refs document");
      require(actual == expected, "reference parser semantics changed");
      Json previous = Json::object();
      for (const auto &[name, key] : expected)
        previous[name] = key_json(key);
      require(encode_refs(actual).dump() == previous.dump(), "reference serialization bytes changed");
    }
    for (const auto &data : std::vector<std::string>{
             "[]", "null", "{", "{} trailing", "{\"a\":null}", "{\"a\":{}}", "{\"../bad\":" + first + "}",
             "{\"a\":{\"kind\":\"unknown\",\"id\":\"" + a.id + "\"}}",
             "{\"a\":{\"kind\":\"chunk\",\"id\":3}}", "{\"a\":{\"kind\":\"chunk\",\"id\":\"bad\"}}",
             "{\"a\":{\"kind\":\"chunk\",\"id\":\"" + a.id + "\",\"extra\":true}}",
             "{\"\xff\":" + first + "}", std::string(130, '[') + "0" + std::string(130, ']')}) {
      fails([&] { (void)previous_parser(data); });
      fails([&] { (void)parse_refs(bytes(data), "invalid refs document"); });
    }
    fails([&] { (void)parse_refs(bytes(std::string(max_object_size + 1, ' ')), "invalid refs document"); });

    Refs expected;
    for (bool encrypted : {false, true}) {
      const auto path = root / std::to_string(encrypted);
      std::optional<Key> key;
      if (encrypted) {
        key.emplace();
        key->fill(37);
      }
      {
        Store store(path, key, true);
        std::istringstream input("retained file with many independent pins");
        const ObjectKey manifest{Kind::manifest, store.ingest(input).first};
        std::vector<RefUpdate> updates;
        expected.clear();
        for (unsigned i = 0; i != 32768; ++i) {
          auto name = "git-live/repository/" + std::to_string(100000 + i);
          updates.push_back({name, {}, manifest});
          expected.emplace(name, manifest);
        }
        require(store.compare_update_refs(updates), "large reference transaction rejected");
        require(store.refs() == expected, "large reference table differs after publication");
        require(text(read_bytes(path / "refs.json")) == encode_refs(expected).dump(),
                "persisted reference representation differs");
        const auto name = expected.begin()->first;
        require(store.compare_update_refs({{name, manifest, {}}}), "large table update rejected");
        expected.erase(name);
      }
      {
        Store reopened(path, key);
        require(reopened.refs() == expected, "large reference table differs after reopen");
        require(reopened.fsck() == 2, "large table lost file dependencies");
      }
      auto original = read_bytes(path / "refs.json");
      atomic_write(path / "refs.json", bytes("{\"bad\":null}"), true);
      fails([&] { Store invalid(path, key); });
      require(text(read_bytes(path / "refs.json")) == "{\"bad\":null}",
              "invalid reference table was overwritten");
      atomic_write(path / "refs.json", original, true);
    }
    std::cout << "Verified large reference tables, parser compatibility, strict validation and reopen\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
