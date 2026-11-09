#include "native_history_fixture.hpp"
#include "test_cases.hpp"
#include <syrnck/bisect.hpp>
#include <iostream>
using namespace native_test;
namespace {
using Edges = std::map<std::string, std::vector<std::string>>;
bool ancestor(const Edges &edges, const std::string &source, const std::string &target) {
  std::vector<std::string> pending{target};
  std::set<std::string> seen;
  while (!pending.empty()) {
    const auto id = pending.back();
    pending.pop_back();
    if (id == source)
      return true;
    if (!seen.insert(id).second)
      continue;
    const auto &parents = edges.at(id);
    pending.insert(pending.end(), parents.begin(), parents.end());
  }
  return false;
}
void histories(const fs::path &root, std::optional<Key> key, unsigned layout) {
  Fixture f(root, key);
  Edges edges;
  auto revision = [&](std::vector<std::string> parents) {
    const auto id =
        f.revision(Json{{"code.cpp", f.file("revision " + std::to_string(edges.size()))}}, Json(parents));
    edges[id] = std::move(parents);
    return id;
  };
  const auto good = revision({});
  const auto left1 = revision({good}), right1 = revision({good});
  const auto fault = revision({left1}), left3 = revision({fault});
  const auto right2 = revision({right1}), right3 = revision({right2});
  const auto merge = revision({left3, right3}), bad = revision({merge});
  f.install(bad);
  f.pack(layout);
  const auto working = text(read_bytes(f.project / "code.cpp"));
  NativeBisect search(f.store);
  const auto original = f.store.refs();
  require(search.status()["active"] == false, "new store has an active bisect");
  auto status = search.start(good, bad);
  require(status["balance"] == 3 && (status["next_revision"] == left3 || status["next_revision"] == right3),
          "merge-DAG bisect did not choose an optimally balanced candidate");
  const auto first_state = status["state"].get<std::string>();
  const auto first_candidate = status["next_revision"].get<std::string>();
  require(f.store.refs().at("heads/main") == original.at("heads/main"), "bisect moved its branch");
  const auto snapshot = f.physical();
  require(search.status() == status && f.physical() == snapshot, "bisect status wrote objects");
  fails([&] { search.start(good, bad); });
  fails([&] { search.mark("good", first_candidate, std::string(64, '0')); });
  fails([&] { search.mark("good", bad, first_state); });
  require(f.physical() == snapshot, "rejected bisect operation wrote objects");
  // Removing the visible branch must not collect the search's initial history.
  f.store.remove_ref("heads/main");
  f.store.gc(true);
  unsigned steps = 0;
  while (status["status"] == "testing") {
    require(++steps < edges.size(), "bisect did not narrow its candidate set");
    const auto candidate = status["next_revision"].get<std::string>();
    require(candidate != status["bad"].get<std::string>(), "bisect suggested a known bad revision");
    const auto result = ancestor(edges, fault, candidate) ? "bad" : "good";
    const auto previous = status["state"].get<std::string>();
    status = search.mark(result, candidate, previous);
    fails([&] { search.mark(result, candidate, previous); });
    f.store.gc(true);
  }
  require(status["status"] == "found" && status["first_bad"] == fault,
          "merge-DAG bisect found the wrong first bad revision");
  require(text(read_bytes(f.project / "code.cpp")) == working, "bisect changed working files");
  const auto ended = search.reset(status["state"].get<std::string>());
  require(ended["active"] == false && search.status()["active"] == false, "bisect did not reset");
  f.store.gc(true);
  for (const auto &[id, parents] : edges) {
    (void)parents;
    const auto metadata = f.store.metadata(Kind::revision, id);
    std::ostringstream bytes;
    f.store.reconstruct(metadata["files"]["code.cpp"]["manifest"].get<std::string>(), bytes);
    require(!bytes.str().empty(), "bisect reset lost retained history");
  }
  // A fresh nonce prevents an old state token from marking another session.
  const auto fresh = search.start(good, bad);
  require(fresh["state"] != first_state, "bisect session reused an old state identity");
  fails([&] { search.mark("good", fresh["next_revision"].get<std::string>(), first_state); });
}
void skip_and_cancel(const fs::path &root) {
  Fixture f(root);
  const auto file = f.file("asset");
  const auto a = f.revision(Json{{"asset", file}});
  const auto b = f.revision(Json{{"asset", file}}, Json::array({a}));
  const auto c = f.revision(Json{{"asset", file}}, Json::array({b}));
  f.install(c);
  NativeBisect search(f.store);
  const auto refs = f.store.refs();
  fails([&] { search.start(a, a); });
  fails([&] { search.start(c, a); });
  std::stop_source stop;
  stop.request_stop();
  fails([&] { search.start(a, c, {stop.get_token()}); });
  require(f.store.refs() == refs, "invalid or pre-cancelled bisect published state");
  auto state = search.start(a, c);
  require(state["next_revision"] == b, "three-commit bisect chose the wrong midpoint");
  const auto current = f.store.refs();
  fails([&] {
    search.mark("skip", b, state["state"].get<std::string>(),
                {{}, [](const ProgressEvent &event) {
                   if (event.phase == ProgressPhase::publishing)
                     throw Cancelled("bisect publication sentinel");
                 }});
  });
  require(f.store.refs() == current, "cancelled bisect mark changed references");
  state = search.mark("skip", b, state["state"].get<std::string>());
  require(state["status"] == "ambiguous" && state["first_bad"].is_null() &&
              state["possible_first_bad"].size() == 2,
          "skipped bisect falsely identified one revision");
  fails([&] { search.mark("good", b, state["state"].get<std::string>()); });
  const auto before = f.store.refs();
  fails([&] {
    search.reset(state["state"].get<std::string>(), {{}, [](const ProgressEvent &event) {
                                                       if (event.phase == ProgressPhase::publishing)
                                                         throw Cancelled("bisect reset sentinel");
                                                     }});
  });
  require(f.store.refs() == before, "cancelled bisect reset removed protection roots");
  search.reset(state["state"].get<std::string>());
}
void corrupt(const fs::path &root) {
  Fixture f(root);
  const auto a = f.revision(Json{{"asset", f.file("a")}});
  const auto b = f.revision(Json{{"asset", f.file("b")}}, Json::array({a}));
  f.install(b);
  NativeBisect search(f.store);
  const auto state = search.start(a, b);
  const auto refs = f.store.refs();
  const auto original = refs.at("bisect/active");
  auto value = parse_json(f.store.get(original));
  value["good"] = Json::array({a, b});
  f.store.set_ref("bisect/active", f.store.put(Kind::chunk, bytes(value.dump())).first);
  fails([&] { search.status(); });
  f.store.set_ref("bisect/active", original);
  f.store.remove_ref("bisect/pin");
  fails([&] { search.status(); });
}
void limits(const fs::path &root) {
  Fixture f(root);
  const auto prepared = std::chrono::steady_clock::now();
  std::string first, last;
  for (unsigned n = 0; n < 4098; ++n) {
    const auto id = f.revision(Json::object(), last.empty() ? Json::array() : Json::array({last}));
    if (first.empty())
      first = id;
    last = id;
  }
  std::cout << Json{{"phase","bisect-limit-fixture"},{"revisions",4098},
      {"elapsed_ms",std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-prepared).count()}}.dump() << std::endl;
  const auto refs = f.store.refs();
  const auto objects = f.store.objects();
  const auto checked = std::chrono::steady_clock::now();
  fails([&] { NativeBisect(f.store).start(first, last); });
  std::cout << Json{{"phase","bisect-limit-check"},
      {"elapsed_ms",std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-checked).count()}}.dump() << std::endl;
  require(f.store.refs() == refs && f.store.objects() == objects, "limited bisect wrote session metadata");
}
} // namespace
int main(int argc,char **argv) {
  Temporary temporary;
  try {
    fixture_test::TestCases cases(fixture_test::CaseSelection::parse(argc,argv));
    for(bool encrypted:{false,true})for(unsigned layout=0;layout<3;++layout)
      cases.add("histories-"+std::to_string(encrypted)+'-'+std::to_string(layout),[encrypted,layout](const fs::path &root) {
        std::optional<Key> key;
        if(encrypted){key.emplace();key->fill(67);}
        histories(root,key,layout);
      });
    cases.add("skip-and-cancel",[](const fs::path &root){skip_and_cancel(root);});
    cases.add("corrupt",[](const fs::path &root){corrupt(root);});
    cases.add("limits",[](const fs::path &root){limits(root);});
    return cases.run(temporary.path);
  } catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}
}
