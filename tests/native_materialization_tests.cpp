#include "native_materialization.hpp"
#include "native_history_fixture.hpp"
#include <iostream>
using namespace native_test;
int main() {
  try {
    const NativeMaterialization full;
    const auto none = NativeMaterialization::from_prefixes({});
    require(full.complete() && !none.complete() && full.includes("Source/a.cpp") &&
                !none.includes("Source/a.cpp") && none.intersects("") && !none.intersects("Source"),
            "complete and explicitly empty materializations were confused");
    require(full.definition().is_null() && none.definition() == Json::array() && full.identity() != none.identity(),
            "all/none materializations share a serialized identity");
    const auto policy = NativeMaterialization::from_prefixes({"Source", "Content/Asset.uasset", "Données"});
    require(policy.includes("source/Code.cpp") && policy.includes("Content/Asset.uasset") &&
                policy.includes("Données/Équipe") && !policy.includes("SourceElse/Code.cpp") &&
                !policy.includes("Content/Other.uasset") && policy.intersects("Content") &&
                policy.intersects("Source/Generated") && !policy.intersects("Plugins"),
            "physical prefix selection confused siblings, ancestors or portable casing");
    require(NativeMaterialization::from_prefixes({"Straße"}).includes("STRASSE/asset"),
            "materialization prefixes disagree with portable Unicode case folding");
    require(policy.includes(".syrnckignore") && policy.includes("Content/.syrnckignore") &&
                policy.includes("Source/Generated/.syrnckignore") && !policy.includes("Plugins/.syrnckignore") &&
                !none.includes(".syrnckignore"), "partial selection omitted required ignore controls");
    fails([&] { policy.includes("Content/.SYRNCKIGNORE"); });
    require(full.includes(".SYRNCKIGNORE"), "complete selection changed legacy control handling");
    const auto equivalent = NativeMaterialization::from_prefixes({"Données", "Content/Asset.uasset", "Source"});
    require(policy.identity() == equivalent.identity() && policy.encode() == equivalent.encode() &&
                NativeMaterialization::decode(bytes(policy.encode())).definition() == policy.definition(),
            "materialization encoding depends on input order or fails round trip");
    fails([&] { full.encode(); });
    for (const auto &bad : {"", ".", "../Source", "/absolute", "Source//Code", "Source\\Code", ".git", ".syrnck", "Content/", "NUL"})
      fails([&] { NativeMaterialization::from_prefixes({bad}); });
    for (const auto &overlap : std::vector<std::vector<std::string>>{
             {"Source", "Source/a"}, {"Source/a", "Source"}, {"Source", "source"},
             {"Données", "Données/child"}, {"Straße", "STRASSE/child"}})
      fails([&] { NativeMaterialization::from_prefixes(overlap); });
    for (const auto &invalid : std::vector<std::string>{"null", "{}", "[]",
             "{\"version\":2,\"type\":\"native-materialization\",\"prefixes\":[]}",
             "{\"version\":1,\"type\":\"native-materialization\",\"prefixes\":null}",
             "{\"version\":1,\"type\":false,\"prefixes\":[]}",
             "{\"version\":1,\"type\":\"native-materialization\",\"prefixes\":[1]}",
             "{\"version\":1,\"type\":\"native-materialization\",\"prefixes\":[\"Z\",\"A\"]}",
             "{\"type\":\"native-materialization\",\"version\":1,\"prefixes\":[]}",
             none.encode() + '\n'})
      fails([&] { NativeMaterialization::decode(bytes(invalid)); });
    std::vector<std::string> many;
    for (unsigned i = 0; i < NativeMaterialization::max_prefixes; ++i) many.push_back("path-" + std::to_string(i));
    require(NativeMaterialization::from_prefixes(many).prefixes().size() == many.size(), "documented prefix count was refused");
    many.push_back("one-too-many");
    fails([&] { NativeMaterialization::from_prefixes(many); });
    std::vector<std::string> large;
    for (unsigned i = 0; i < 400; ++i) large.push_back("root-" + std::to_string(i) + '/' + std::string(200, 'x'));
    fails([&] { NativeMaterialization::from_prefixes(large); });
    const std::map<std::string, int> logical{{"Source/a", 1}, {"Content/Asset.uasset", 2}, {"Content/Other.uasset", 3}, {"Plugins/Test", 4}};
    auto physical = logical;
    project_native_materialization(physical, policy);
    require(physical == std::map<std::string, int>{{"Source/a", 1}, {"Content/Asset.uasset", 2}} && logical.size() == 4,
            "physical projection changed logical entries or included omitted files");
    project_native_materialization(physical, none);
    require(physical.empty(), "empty materialization retained physical files");
    for (unsigned mask = 0; mask < 256; ++mask) {
      std::vector<std::string> selected;
      for (unsigned bit = 0; bit < 8; ++bit) if (mask & (1U << bit)) selected.push_back("root-" + std::to_string(bit));
      const auto materialization = NativeMaterialization::from_prefixes(selected);
      for (unsigned bit = 0; bit < 12; ++bit) {
        const auto root = "root-" + std::to_string(bit);
        const bool expected = bit < 8 && (mask & (1U << bit));
        require(materialization.includes(root + "/nested/file") == expected &&
                    materialization.intersects(root) == expected && !materialization.includes(root + "-sibling/file"),
                "prefix projection disagrees with independent selected-root membership");
      }
    }
    std::cout << "Canonical bounded physical selections preserve logical trees and portable path relationships\n";
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
