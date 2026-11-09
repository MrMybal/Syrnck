#include "native_history_fixture.hpp"
#include "test_cases.hpp"
#include <iostream>
using namespace native_test;
int main() {
  Temporary temporary;
  try {
    const auto rejected = [](auto function) {
      bool failed = false;
      try {function();} catch (const std::invalid_argument &) {failed = true;}
      require(failed,"invalid case selection was accepted");
    };
    for (std::size_t size = 1; size <= 257; ++size) {
      for (std::size_t count = 1; count <= std::min<std::size_t>(64,size); ++count) {
        std::vector<unsigned> covered(size);
        for (std::size_t index = 0; index < count; ++index) {
          const auto [first,last] = fixture_test::CaseSelection{index,count}.range(size);
          require(first < last && last <= size, "partition empty or outside case range");
          for (auto at = first; at < last; ++at) ++covered[at];
        }
        require(std::all_of(covered.begin(),covered.end(),[](auto value){return value==1;}),
            "partition dropped or repeated a scenario");
      }
    }
    fixture_test::TestCases cases;
    rejected([&]{ cases.add("../outside",[](auto &){}); });
    cases.add("first",[](const fs::path &root){ fs::create_directories(root); atomic_write(root/"data",bytes("first"),false); });
    rejected([&]{ cases.add("first",[](auto &){}); });
    cases.add("second",[&](const fs::path &root){
      require(!fs::exists(temporary.path/"first"),"successful case retained its whole fixture until suite teardown");
      fs::create_directories(root); atomic_write(root/"data",bytes("second"),false);
    });
    require(cases.run(temporary.path)==0 && fs::is_empty(temporary.path),"case cleanup incomplete");
    for (auto value : {fixture_test::CaseSelection{0,0}, fixture_test::CaseSelection{2,2}, fixture_test::CaseSelection{0,64}})
      rejected([&]{value.range(2);});
    const auto parse = [](std::vector<std::string> values) {
      std::vector<char *> arguments;
      for (auto &value : values) arguments.push_back(value.data());
      return fixture_test::CaseSelection::parse(static_cast<int>(arguments.size()),arguments.data());
    };
    const auto selected = parse({"test","--list","--shard","1/3"});
    require(selected.list && selected.index==1 && selected.count==3,"selection arguments differ");
    for (const auto &arguments : std::vector<std::vector<std::string>>{
        {"test","--shard"},{"test","--list","--list"},{"test","--unknown"},
        {"test","--shard","1/0"},{"test","--shard","-1/2"},{"test","--shard","2/2"},
        {"test","--shard","0/65"},{"test","--shard","0/1junk"},
        {"test","--shard","0/1","--shard","0/1"}})
      rejected([&]{parse(arguments);});
    std::cout<<"Complete case partitions and owned incremental cleanup verified\n";
  } catch (const std::exception &error) { std::cerr<<error.what()<<'\n';return 1; }
}
