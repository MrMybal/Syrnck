#include "native_history_fixture.hpp"
#include <syrnck/backup.hpp>
#include <iostream>
using namespace native_test;
namespace {
const std::string owner(64,'a');
bool armed=false,tracing=false,racing=false,race_injected=false,race_copy=false;
fs::path race_original;
bool injecting=false;
Json *race_info=nullptr;
std::string wanted_phase,wanted_kind,race_phase;
std::size_t wanted=0,seen=0;
Json events=Json::array();
std::map<std::pair<std::string,std::string>,std::size_t> counts;
fs::path fixture_root;
std::optional<Key> encryption(const Json &info) {
 if(!info["encrypted"].get<bool>())return {};
 Key key;key.fill(79);return key;
}
fs::path directory(const fs::path &root,const Json &info){return root/(info["scope"]=="primary"?"primary":"edit");}
void select(Store &store,const Json &info){store.use_native_workspace(info["scope"]=="primary"?"":"edit");}
std::string before_path(const std::string &layout) {return layout=="reverse"?"Source/item/leaf":layout=="case"?"Source/OLD/item":"Source/item";}
std::string target_path(const Json &info) {const auto layout=info.value("layout",info["transition"].get<bool>()?std::string("transition"):std::string("ordinary"));return layout=="transition"?"Source/item/leaf":layout=="case"?"Source/old/item":"Source/item";}
void prepare(const fs::path &root,bool encrypted,const std::string &scope,const std::string &layout) {
 const bool transition=layout=="transition";
 const auto before=before_path(layout);
 const auto after=layout=="transition"?"Source/item/leaf":layout=="case"?"Source/old/item":"Source/item";
 Fixture f(root/"primary",encryption(Json{{"encrypted",encrypted}}));
 const auto files=Json{{".syrnckignore",f.file("*.keep\n")},{before,f.file("before")},
     {"Source/steady",f.file("unchanged")},{"Content/hidden",f.file("hidden")}};
 const auto original=f.revision(files);f.install(original);
 auto next=files;if(before!=after)next.erase(before);next[after]=f.file("incoming");
 auto target=f.revision(next,Json::array({original}));Key signing;signing.fill(78);
 f.store.use_native_identity({"Recovery test","recovery@example.invalid",0},SigningKey::from_seed(signing));
 target=f.store.put(Kind::revision,bytes(f.store.native_commit_metadata(f.store.metadata(Kind::revision,target)).dump())).first.id;
 f.store.set_ref("incoming",{Kind::revision,target});f.store.pack();
 atomic_write(root/"fixture.json",bytes(Json{{"encrypted",encrypted},{"scope",scope},{"transition",transition},{"layout",layout},
     {"original",original},{"target",target},{"bound",false}}.dump()),false);
}
void bind(Store &store,const fs::path &root,Json &info) {
 const auto working=directory(root,info);
 if(info["scope"]!="primary") {
  store.register_partial_native_workspace(root/"primary","edit",working,{"Source"});
  store.create_native_branch("edit",info["original"].get<std::string>());
  store.use_native_workspace("edit");store.switch_directory(working,"edit");
 }
 bool injected=false;
 try {
  store.checkout_directory_owned(working,store.native_target(),info["target"].get<std::string>(),owner,{{},[&](const ProgressEvent &event){
   if(!injected&&event.phase==ProgressPhase::publishing&&event.path=="checkout") {
    injected=true;atomic_write(working/path_from_utf8(target_path(info)),bytes("local changed item"),true);
    atomic_write(working/"Source/steady",bytes("local changed steady"),true);
   }
  }});
 } catch(const Error &) {require(injected&&store.checkout_status()["pending"]==true,"fixture interruption failed");}
 require(injected&&store.checkout_status()["pending"]==true,"fixture completed instead of preserving pending edit");
 info["bound"]=true;atomic_write(root/"fixture.json",bytes(info.dump()),true);
}
Json verify(Store &store,const fs::path &root,const Json &info,bool complete) {
 const auto pending=store.checkout_status();
 store.fsck();
 (void)store.closure({{Kind::revision,info["original"].get<std::string>()},{Kind::revision,info["target"].get<std::string>()}},true);
 std::set<std::string> recovered, wanted_content{"local changed item","local changed steady"};
 if(info.contains("expected_extra"))wanted_content.insert(info["expected_extra"].get<std::string>());
 const auto working=directory(root,info);
 if(fs::exists(working))for(fs::recursive_directory_iterator i(working),end;i!=end;++i) {
  if(i->path().filename()==".syrnck"){i.disable_recursion_pending();continue;}
  if(i->is_regular_file()) {
   const auto text_value=text(read_bytes(i->path()));
   if(wanted_content.contains(text_value))recovered.insert(text_value);
  }
 }
 std::size_t copies=0,versions=0;
 const auto prefix="archive/checkout-edits/";
 for(const auto &[name,key]:store.refs())if((name.starts_with(prefix)||name.starts_with("archive/checkout-edit-versions/"))&&key.kind==Kind::revision) {
  if(name.starts_with("archive/checkout-edit-versions/"))++versions;
  (void)store.closure({key},true);
  const auto revision=store.metadata(Kind::revision,key.id);
  for(const auto &[path,entry]:revision["files"].items()) {
   (void)path;const auto metadata=store.metadata(Kind::manifest,entry["manifest"].get<std::string>());
   for(const auto &value:wanted_content)
    if(metadata["file_hash"]==hash(bytes(value)))recovered.insert(value);
  }
 }
 require(recovered==wanted_content,"process kill lost unarchived local bytes");
 if(pending["pending"].get<bool>())require(pending["owner"]==owner&&pending["target"]==info["target"],"process kill changed owned target");
 if(complete) {
  require(!pending["pending"].get<bool>()&&store.native_head()["revision"]==info["target"],"preservation did not finish signed target");
  require(text(read_bytes(working/path_from_utf8(target_path(info))))=="incoming"&&
      text(read_bytes(working/"Source/steady"))=="unchanged","preservation installed wrong canonical bytes");
  if(info["scope"]=="partial")require(!fs::exists(working/"Content"),"preservation expanded partial workspace");
  for(fs::recursive_directory_iterator i(working),end;i!=end;++i) {
   if(i->path().filename()==".syrnck"){i.disable_recursion_pending();continue;}
   if(i->is_regular_file()&&utf8(i->path().filename()).find(".syrnck-conflict-")!=std::string::npos)++copies;
  }
  require(copies==info.value("expected_copies",info.contains("foreign_copy")?3u:2u),"restart duplicated or lost conflict copies");
  if(info.contains("foreign_copy"))require(text(read_bytes(path_from_utf8(info["foreign_copy"].get<std::string>())))=="foreign conflict copy", "foreign copy was replaced");
  store.gc(true);store.fsck();
 }
 return Json{{"verified",true},{"pending",pending["pending"]},{"copies",copies},{"versions",versions}};
}
}
namespace syrnck::testing {
void io_checkpoint(std::string_view phase,const fs::path &path) {
 if(injecting)return;
 const auto relative=path.lexically_relative(fixture_root/"primary/.syrnck/objects");
 const auto kind=path.filename()=="refs.json"?"refs":!relative.empty()&&*relative.begin()!=".."?utf8(*relative.begin()):"filesystem";
 if(racing&&!race_injected&&phase==race_phase) {
  race_injected=true;injecting=true;
  atomic_write(race_copy?path:race_original,bytes(race_copy?"later conflict copy edit":"later original edit"),true);
  if(race_info) {
    (*race_info)["copy_raced"]=true;
    (*race_info)["expected_extra"]=race_copy?"later conflict copy edit":"later original edit";
    (*race_info)["expected_copies"]=race_copy?2:3;
    atomic_write(fixture_root/"fixture.json",bytes(race_info->dump()),true);
  }
  injecting=false;
 }
 if(tracing){events.push_back(Json{{"phase",phase},{"kind",kind},{"occurrence",++counts[{std::string(phase),kind}]}});return;}
 if(!armed||phase!=wanted_phase||kind!=wanted_kind||++seen!=wanted)return;
 armed=false;std::cout<<Json{{"phase",phase},{"kind",kind},{"occurrence",seen}}.dump()<<std::endl;
 std::string line;require(static_cast<bool>(std::getline(std::cin,line))&&line=="continue","preservation checkpoint closed");
}
}
int main(int argc,char **argv) {
 try {
  require(argc>=3,"checkout edit probe expects mode and root");
  const std::string mode=argv[1];const auto root=path_from_utf8(argv[2]);fixture_root=root;
  if(mode=="prepare"||mode=="prepare-copy-race") {
   require(argc==6,"checkout edit fixture expects encryption scope transition");
   prepare(root,std::string_view(argv[3])=="encrypted",argv[4],argv[5]);
   if(mode=="prepare-copy-race") {
    auto info=parse_json(read_bytes(root/"fixture.json"));info["copy_race"]=true;
    atomic_write(root/"fixture.json",bytes(info.dump()),true);
   }
   return 0;
  }
  auto info=parse_json(read_bytes(root/"fixture.json"));Store store(root/"primary/.syrnck",encryption(info));
  if(!info["bound"].get<bool>())bind(store,root,info);else select(store,info);
  require(store.native_identity().is_null(),"recovery loaded a private signing key");
  if(mode=="inject-collision") {
   for(const auto &[name,key]:store.refs())if(name.starts_with("archive/checkout-edits/")&&name.ends_with("/state")) {
    const auto edit=parse_json(store.get(key));const auto copy=directory(root,info)/path_from_utf8(edit["copy"].get<std::string>());
    atomic_write(copy,bytes("foreign conflict copy"),false);
    info["foreign_copy"]=utf8(copy);atomic_write(root/"fixture.json",bytes(info.dump()),true);
    std::cout<<Json{{"path",utf8(copy)}}.dump()<<'\n';return 0;
   }
   throw Error("no prepared preservation to collide with");
  }
  if(mode=="backup") {
    require(store.checkout_status()["pending"]==false,"backup fixture has active checkout");
    Key backup_key,restore_key;backup_key.fill(84);restore_key.fill(85);
    create_backup(store,root/"backup",backup_key);Store saved(root/"backup",backup_key);verify_backup(saved);
    restore_backup(saved,root/"restored",restore_key);Store restored(root/"restored",restore_key);restored.fsck();
    require(restored.checkout_status()["pending"]==false,"restored archives activated a journal");
    const auto copied=restored.refs();std::size_t versions=0;
    for(const auto &[name,value]:store.refs())if(name.starts_with("archive/checkout-edits/")||name.starts_with("archive/checkout-edit-versions/")) {
      require(copied.contains(name)&&copied.at(name)==value,"backup dropped preservation root");
      if(name.starts_with("archive/checkout-edit-versions/")&&value.kind==Kind::revision) {
        ++versions;const auto revision=restored.metadata(Kind::revision,value.id);
        const auto manifest=revision["files"].begin().value()["manifest"].get<std::string>();
        const auto output=root/("backup-version-"+value.id);restored.restore_file(manifest,output);
        require(text(read_bytes(output))==info["expected_extra"].get<std::string>(),"restored backup lost updated copy bytes");
      }
    }
    require(versions>0,"backup has no additional copy versions");
    std::cout<<Json{{"verified",true},{"versions",versions}}.dump()<<'\n';return 0;
  }
  if((mode=="trace"||mode=="run")&&info.value("copy_race",false)&&!info.value("copy_raced",false)) {
    racing=true;race_copy=true;race_phase="checkout-edit.moved";race_info=&info;
  }
  if(mode=="trace")tracing=true;
  else if(mode=="run") {require(argc==6,"checkout edit checkpoint expects phase kind occurrence");wanted_phase=argv[3];wanted_kind=argv[4];wanted=std::stoull(argv[5]);armed=true;}
  else if(mode=="race") {
   require(argc==5,"checkout edit race expects checkpoint and location");racing=true;race_phase=argv[3];race_copy=std::string_view(argv[4])=="copy";
   race_original=directory(root,info)/path_from_utf8(target_path(info));
   info["expected_extra"]=race_copy?"later conflict copy edit":"later original edit";
   info["expected_copies"]=race_copy?2:3;atomic_write(root/"fixture.json",bytes(info.dump()),true);
  }
  else require(mode=="verify"||mode=="resume","unknown checkout edit probe mode");
  if(mode!="verify"&&store.checkout_status()["pending"].get<bool>())
   store.recover_checkout_owned_preserving_files(directory(root,info),store.native_target(),owner);
  if(mode=="trace")std::cout<<events.dump()<<'\n';
  else if(mode=="run")require(!armed,"checkout edit checkpoint not reached");
  else {
   if(racing)require(race_injected,"edit race checkpoint was not reached");
   std::cout<<verify(store,root,info,mode=="resume"||mode=="race").dump()<<'\n';
  }
 }catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}
}
