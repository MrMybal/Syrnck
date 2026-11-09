#include "native_history_fixture.hpp"
#include "test_cases.hpp"
#include <syrnck/backup.hpp>
#include <iostream>
using namespace native_test;
namespace {
const std::string owner(64, 'a');
void scenario(const fs::path &root, bool encrypted, const std::string &scope, unsigned transition, unsigned phase) {
  std::optional<Key> key; if (encrypted) {key.emplace(); key->fill(79);}
  const bool partial = scope == "partial" || scope == "partial-detached";
  const bool detached = scope == "detached" || scope == "partial-detached";
  const auto project = root / "primary", directory = scope == "primary" ? project : root / "edit";
  const std::string before = transition==2 ? "Source/item/leaf" : transition==3 ? "Source/OLD/item" : "Source/item";
  const std::string item = transition==1 ? "Source/item/leaf" : transition==3 ? "Source/old/item" : "Source/item";
  std::string original, candidate, branch;
  {
    Fixture f(project,key);
    const auto files = Json{{".syrnckignore",f.file("*.keep\n")}, {before,f.file("before")},
        {"Source/steady",f.file("unchanged",true)}, {"Content/hidden",f.file("hidden")}};
    original=f.revision(files); f.install(original);
    auto next=files; if(before!=item)next.erase(before); next[item]=f.file("incoming");
    candidate=f.revision(next,Json::array({original}));
    Key signing; signing.fill(78);
    f.store.use_native_identity({"Archive test","archive@example.invalid",0},SigningKey::from_seed(signing));
    candidate=f.store.put(Kind::revision,bytes(f.store.native_commit_metadata(f.store.metadata(Kind::revision,candidate)).dump())).first.id;
    f.store.set_ref("incoming",{Kind::revision,candidate});
    if(scope!="primary") {
      if(partial)f.store.register_partial_native_workspace(project,"edit",directory,{"Source"});
      else f.store.register_native_workspace(project,"edit",directory);
      if(!detached)f.store.create_native_branch("edit",original);
      f.store.use_native_workspace("edit");
      if(detached)f.store.detach_checkout(directory,original);else f.store.switch_directory(directory,"edit");
    }
    branch=f.store.native_target();
    atomic_write(directory/"unrelated.keep",bytes("ignored work"),false);
    bool injected=false;
    try {
      f.store.checkout_directory_owned(directory,branch,candidate,owner,{{},[&](const ProgressEvent &event){
        if(injected)return;
        if((phase==0 && event.phase==ProgressPhase::reconstructing && event.bytes>0) ||
           (phase==1 && event.phase==ProgressPhase::exporting)) {
          injected=true; throw Cancelled("test pause before file application");
        }
        if(phase==2 && event.phase==ProgressPhase::publishing && event.path=="checkout") {
          injected=true;
          atomic_write(directory/path_from_utf8(item),bytes("local changed item"),true);
          atomic_write(directory/"Source/steady",bytes("local changed steady"),true);
        }
      }});
      throw Error("test did not interrupt checkout");
    } catch(const Error &) {require(injected,"test interruption not reached");}
    if(phase!=2) {
      atomic_write(directory/path_from_utf8(before),bytes("local changed item"),true);
      atomic_write(directory/"Source/steady",bytes("local changed steady"),true);
    }
    require(f.store.checkout_status()["pending"]==true,"test has no active checkout");
    const auto unchanged=f.physical();
    fails([&]{f.store.recover_checkout_owned_preserving_files(directory,branch,std::string(64,'b'));});
    require(f.physical()==unchanged,"foreign edit preservation mutated files");
    if (!encrypted && scope=="primary" && !transition && phase==2) {
      bool changed=false;
      fails([&]{f.store.recover_checkout_owned_preserving_files(directory,branch,owner,{{},[&](const ProgressEvent &event){
        if(!changed && event.phase==ProgressPhase::publishing && event.path=="refs") {
          changed=true;f.store.set_ref("heads/main",{Kind::revision,candidate});
        }
      }});});
      require(changed && text(read_bytes(directory/"Source/item"))=="local changed item",
          "stale preservation moved working bytes after a reference callback");
      for(const auto &[name,value]:f.store.refs()) {
        (void)value;require(!name.starts_with("archive/checkout-edits/"),"stale callback published preservation intent");
      }
      f.store.set_ref("heads/main",{Kind::revision,original});
    }
    f.pack(encrypted ? 2 : 1);
  }
  {
    Store store(project/".syrnck",key);store.use_native_workspace(scope=="primary"?"":"edit");
    require(store.native_identity().is_null(),"recovery imported signing key");
    const auto report=store.recover_checkout_owned_preserving_files(directory,branch,owner);
    require(report["state"]=="committed"&&report["target"]==candidate&&report["preserved_edits"].size()==2,
        "owned preservation did not finish exact signed candidate with two copies");
    require(text(read_bytes(directory/path_from_utf8(item)))=="incoming"&&
        text(read_bytes(directory/"Source/steady"))=="unchanged"&&
        text(read_bytes(directory/"unrelated.keep"))=="ignored work","preservation changed target or ignored work");
    if(partial)require(!fs::exists(directory/"Content"),"preservation expanded hidden files");
    std::set<std::string> content;
    for(const auto &edit:report["preserved_edits"]) {
      const auto copy=edit["copy_path"].get<std::string>();content.insert(text(read_bytes(directory/path_from_utf8(copy))));
      const auto revision=store.metadata(Kind::revision,edit["revision"].get<std::string>());
      require(revision["parents"]==Json::array({candidate}),"archive lost incoming causal parent");
    }
    require(content==std::set<std::string>{"local changed item","local changed steady"},"preservation discarded an edit");
    const auto signed_target=store.metadata(Kind::revision,candidate);
    require(signed_target["version"]==2&&signed_target["signature"].is_object(),"preservation rewrote signed target");
    store.gc(true);store.fsck();
    for(const auto &edit:report["preserved_edits"]) {
      const auto revision=store.metadata(Kind::revision,edit["revision"].get<std::string>());
      const auto copy=edit["copy_path"].get<std::string>();
      const auto saved=root/("restore-"+hash(bytes(copy)));
      store.restore_file(revision["files"][copy]["manifest"].get<std::string>(),saved);
      require(content.contains(text(read_bytes(saved))),"GC lost preserved edit blocks");
    }
    if(scope=="primary"&&transition==0&&phase==2) {
      Key backup_key, restored_key;backup_key.fill(84);restored_key.fill(85);
      create_backup(store,root/"backup",backup_key);
      Store backup(root/"backup",backup_key);verify_backup(backup);
      restore_backup(backup,root/"restored",restored_key);
      Store restored(root/"restored",restored_key);restored.fsck();
      require(restored.checkout_status()["pending"]==false,"backup activated a historical edit intent");
      const auto refs=restored.refs();
      for(const auto &[name,value]:store.refs())if(name.starts_with("archive/checkout-edits/")||name.starts_with("archive/checkout-edit-versions/"))
        require(refs.contains(name)&&refs.at(name)==value,"portable backup dropped edit archive roots");
      for(const auto &edit:report["preserved_edits"]) {
        const auto copy=edit["copy_path"].get<std::string>();
        const auto archived=restored.metadata(Kind::revision,edit["revision"].get<std::string>());
        const auto output=root/("from-backup-"+hash(bytes(copy)));
        restored.restore_file(archived["files"][copy]["manifest"].get<std::string>(),output);
        require(content.contains(text(read_bytes(output))),"independently keyed backup lost edited bytes");
      }
    }
  }
}
}
int main(int argc, char **argv) {
 try {
  if (argc > 1 && std::string_view(argv[1]) == "interrupt-folder") {
    require(argc == 9 && std::string_view(argv[1]) == "interrupt-folder", "invalid checkout edit test helper arguments");
    const auto directory=path_from_utf8(argv[2]), storage=path_from_utf8(argv[3]);
    const std::string branch=argv[4], candidate=argv[5], service=argv[6], relative=argv[7];
    validate_path(relative); std::optional<Key> key;
    if(std::string_view(argv[8])=="encrypted") {key.emplace();key->fill(83);}
    Store store(storage,key); bool injected=false;
    try {
      store.checkout_directory_owned(directory,branch,candidate,service,{{},[&](const ProgressEvent &event){
        if(event.phase==ProgressPhase::publishing && event.path=="checkout" && !injected) {
          injected=true;atomic_write(directory/path_from_utf8(relative),bytes("edited during live native application"),true);
        }
      }});
    } catch(const Error &) {
      require(injected&&store.checkout_status()["pending"]==true,"test helper failed before injected edit");
      std::cout<<Json{{"pending",true}}.dump()<<'\n'; return 0;
    }
    throw Error("test helper checkout accepted concurrent edit");
  }
  Temporary temporary;
  fixture_test::TestCases cases(fixture_test::CaseSelection::parse(argc,argv));
  for(bool encrypted:{false,true})for(const auto *scope:{"primary","linked","partial","detached","partial-detached"})
    for(unsigned transition=0;transition!=4;++transition)for(unsigned phase=0;phase!=3;++phase) {
      const auto label=std::to_string(encrypted)+'-'+scope+'-'+std::to_string(transition)+'-'+std::to_string(phase);
      cases.add(label,[encrypted,scope,transition,phase](const fs::path &root){scenario(root,encrypted,scope,transition,phase);});
    }
  return cases.run(temporary.path);
 } catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}
}
