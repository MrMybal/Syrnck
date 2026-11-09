#include "native_history_fixture.hpp"
#include <syrnck/native_sync.hpp>
#include <iostream>
using namespace native_test;
namespace {
void scenario(const fs::path &root,bool encrypted,bool transition) {
 std::optional<Key> key;if(encrypted){key.emplace();key->fill(82);}
 const net::Remote remote("http://127.0.0.1:9",std::string(32,'t'));
 std::string original,candidate,service;
 const std::string path=transition?"Source/item/leaf":"Source/item";
 {
  Fixture f(root,key);
  original=f.revision(Json{{"Source/item",f.file("before")}});f.install(original);
  candidate=f.revision(Json{{path,f.file("incoming")}},Json::array({original}));
  Key signing;signing.fill(81);
  f.store.use_native_identity({"Agent","agent@example.invalid",0},SigningKey::from_seed(signing));
  candidate=f.store.put(Kind::revision,bytes(f.store.native_commit_metadata(f.store.metadata(Kind::revision,candidate)).dump())).first.id;
  service=hash(bytes(Json{{"version",1},{"type","native-push-sync"},{"workspace",""},
      {"target","main"},{"remote_branch","main"},{"endpoint",remote.endpoint_fingerprint()}}.dump()));
  f.store.set_ref("sync/push/"+service+"/observed",{Kind::revision,candidate});
  bool edited=false;
  fails([&]{f.store.checkout_directory_owned(root,"main",candidate,service,{{},[&](const ProgressEvent &event){
   if(event.phase==ProgressPhase::publishing&&event.path=="checkout"&&!edited) {
    edited=true;atomic_write(root/path_from_utf8(path),bytes("live editor bytes"),true);
   }
  }});});
  require(edited&&f.store.checkout_status()["pending"]==true,"agent fixture not interrupted after writes");
 }
 Store store(root/".syrnck",key);require(store.native_identity().is_null(),"agent imported private key");
 net::NativeSync strict(store,root,{"main",false,true,true,false});
 fails([&]{strict.tick(store,remote);});
 require(store.native_head()["revision"]==original&&text(read_bytes(root/path_from_utf8(path)))=="live editor bytes",
     "strict agent overwrote active edit");
 net::NativeSync copying(store,root,{"main",false,true,true,true});
 const auto result=copying.tick(store,remote);
 require(result["state"]=="recovered"&&result["checkout"]["target"]==candidate&&result["checkout"]["preserved_edits"].size()==1,
     "explicit conflict-copy agent did not resume exact owned target offline");
 const auto copy=result["checkout"]["preserved_edits"][0]["copy_path"].get<std::string>();
 require(text(read_bytes(root/path_from_utf8(path)))=="incoming"&&text(read_bytes(root/path_from_utf8(copy)))=="live editor bytes",
     "agent recovery discarded active editor data");
 require(store.metadata(Kind::revision,candidate)["signature"].is_object(),"agent rewrote prepared signed target");
 store.gc(true);store.fsck();
}
}
int main(){
 try {
  Temporary temporary;
  for(bool encrypted:{false,true})for(bool transition:{false,true})
   scenario(temporary.path/(std::to_string(encrypted)+'-'+std::to_string(transition)),encrypted,transition);
  std::cout<<"Offline native agent preserves active edits only under explicit conflict-copy policy\n";
 }catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}
}
