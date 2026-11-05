#include "native_history_fixture.hpp"
#include "native_merge.hpp"
#include <iostream>
using namespace native_test;
namespace {
void run(const fs::path &root, std::optional<Key> key, unsigned layout) {
  Fixture f(root, key);
  const auto original = f.file("a\r\nb\r\nc"), ours = f.file("A\r\nb\r\nc", true),
             theirs = f.file("a\r\nb\r\nC"), overlap = f.file("B\r\nb\r\nc");
  const auto base = f.revision(Json{{"code.cpp", original}});
  const auto current = f.revision(Json{{"code.cpp", ours}}, Json::array({base}));
  const auto source =
      f.revision(Json{{"code.cpp", theirs}, {"independent", f.file("incoming asset")}}, Json::array({base}));
  const auto conflicting = f.revision(Json{{"code.cpp", overlap}}, Json::array({base}));
  f.install(current);
  f.store.set_ref("heads/feature", {Kind::revision, source});
  f.pack(layout);
  const auto before = f.physical();
  require(f.store.plan_merge(root, "main", source)["ready"] == false, "text merge became implicit");
  NativeMergeOptions options;
  options.text_merge.emplace();
  const auto plan = prepare_native_merge(f.store, root, "main", source, options, true, {});
  const auto manifest = plan.files.at("code.cpp").manifest;
  require(plan.report["ready"] == true && plan.report["text_merge"]["files"][0]["status"] == "clean" &&
              plan.report["text_merge"]["generated_manifests"] == 1 && plan.generated.size() == 1 &&
              plan.files.at("code.cpp").executable && !f.store.contains({Kind::manifest, manifest}) &&
              f.physical() == before,
          "automatic text plan wrote objects or lost content/mode");
  const auto blocked = f.store.plan_merge(root, "main", conflicting, options);
  require(blocked["ready"] == false && blocked["conflicts"][0]["reason"] == "content" &&
              blocked["text_merge"]["files"][0]["status"] == "conflict" && blocked["workspace"].is_null(),
          "automatic merge hid a text conflict");
  auto limited = options;
  limited.text_merge->max_file_bytes = 1;
  require(f.store.plan_merge(root, "main", source, limited)["text_merge"]["files"][0]["reason"] ==
              "file_limit",
          "native merge ignored file budget");
  limited = options;
  limited.text_merge->max_work = 1;
  require(f.store.plan_merge(root, "main", source, limited)["text_merge"]["files"][0]["reason"] ==
              "work_limit",
          "native merge ignored work budget");
  auto explicit_choice = options;
  explicit_choice.resolutions = NativeResolutionSet::parse(plan.report["resolution_context"]);
  explicit_choice.resolutions->files["code.cpp"] = {NativeResolutionChoice::current};
  const auto manual = prepare_native_merge(f.store, root, "main", source, explicit_choice, false, {});
  require(manual.report["ready"] == true && manual.report["text_merge"]["attempted_files"] == 0 &&
              manual.generated.empty() &&
              manual.files.at("code.cpp").manifest == ours["manifest"].get<std::string>(),
          "automatic text merge overrode an explicit resolution");
  std::stop_source stop;
  fails([&] {
    f.store.merge_directory(root, "main", current, source, "cancel before candidate", options,
                            {stop.get_token(), [&](const ProgressEvent &event) {
                               if (event.phase == ProgressPhase::ingesting)
                                 stop.request_stop();
                             }});
  });
  require(f.physical() == before, "cancelled text materialization published objects or refs");
  f.write("code.cpp", "dirty");
  const auto dirty = f.physical();
  fails([&] { f.store.merge_directory(root, "main", current, source, "dirty", options); });
  require(f.physical() == dirty && !f.store.contains({Kind::manifest, manifest}),
          "blocked workspace wrote merged objects");
  f.install(current);
  const auto applied = f.store.merge_directory(root, "main", current, source, "automatic text", options);
  const auto next = applied["target"].get<std::string>();
  require(text(read_bytes(root / "code.cpp")) == "A\r\nb\r\nC" &&
              f.store.metadata(Kind::revision, next)["files"]["code.cpp"]["manifest"] == manifest &&
              f.store.metadata(Kind::revision, next)["parents"] == Json::array({current, source}) &&
              f.store.refs().at("heads/feature").id == source,
          "applied text differs from virtual plan");
  f.store.remove_ref("heads/feature");
  f.store.gc(true);
  std::ostringstream output;
  f.store.reconstruct(manifest, output);
  require(output.str() == "A\r\nb\r\nC" &&
              f.store.metadata(Kind::revision, current)["files"].contains("code.cpp") &&
              f.store.metadata(Kind::revision, source)["files"].contains("code.cpp"),
          "GC lost merged text or parents");

  // Interrupt a second application after the recoverable journal exists.
  f.install(current);
  stop = std::stop_source{};
  fails([&] {
    f.store.merge_directory(root, "main", current, source, "recover text", options,
                            {stop.get_token(), [&](const ProgressEvent &event) {
                               if (event.phase == ProgressPhase::exporting && event.bytes)
                                 stop.request_stop();
                             }});
  });
  require(f.store.checkout_status()["pending"] == true, "text checkout did not retain a recovery journal");
  f.store.recover_checkout();
  require(text(read_bytes(root / "code.cpp")) == "A\r\nb\r\nC", "text checkout recovery lost bytes");
}
void blocked_kinds(const fs::path &root) {
  Fixture f(root);
  const auto a = f.file(std::string("a\0", 2)), b = f.file(std::string("b\0", 2)),
             c = f.file(std::string("c\0", 2));
  const auto base = f.revision(Json{{"asset", a}}),
             current = f.revision(Json{{"asset", b}}, Json::array({base})),
             source = f.revision(Json{{"asset", c}}, Json::array({base}));
  f.install(current);
  NativeMergeOptions options;
  options.text_merge.emplace();
  require(f.store.plan_merge(root, "main", source, options)["text_merge"]["files"][0]["status"] == "binary",
          "binary content merged automatically");
  const auto deletion = f.revision(Json::object(), Json::array({base}));
  const auto removed = f.store.plan_merge(root, "main", deletion, options);
  require(removed["ready"] == false && removed["text_merge"]["attempted_files"] == 0 &&
              removed["conflicts"][0]["reason"] == "modify_delete",
          "text merge changed presence rules");
  const auto before = f.physical();
  options.text_merge->max_lines = 0;
  fails([&] { f.store.plan_merge(root, "main", source, options); });
  require(f.physical() == before, "invalid merge budget wrote data");
}
} // namespace
int main() {
  Temporary tmp;
  try {
    for (bool encrypted : {false, true})
      for (unsigned layout = 0; layout < 3; ++layout) {
        std::optional<Key> key;
        if (encrypted) {
          key.emplace();
          key->fill(61);
        }
        run(tmp.path / (std::to_string(encrypted) + '-' + std::to_string(layout)), key, layout);
      }
    blocked_kinds(tmp.path / "blocked");
    std::cout << "Opt-in automatic text merge, virtual plans, explicit precedence, atomic recovery and "
                 "retained history\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
