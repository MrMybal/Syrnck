#include "native_history_fixture.hpp"
#include "cli_keys.hpp"
#include <syrnck/keys.hpp>
#include <chrono>
#include <iostream>
#ifdef _WIN32
#include <Windows.h>
#else
#include <cstdlib>
#endif
using namespace native_test;
namespace {
void environment(const std::string &name, const std::optional<std::string> &value) {
#ifdef _WIN32
  const auto wide_name = path_from_utf8(name).native();
  const auto wide_value = value ? path_from_utf8(*value).native() : std::wstring{};
  SetLastError(ERROR_SUCCESS);
  const auto ok=SetEnvironmentVariableW(wide_name.c_str(), value ? wide_value.c_str() : nullptr);
  require(ok != 0 || (!value && GetLastError() == ERROR_ENVVAR_NOT_FOUND), "cannot set test environment");
#else
  require((value ? ::setenv(name.c_str(),value->c_str(),1) : ::unsetenv(name.c_str())) == 0, "cannot set test environment");
#endif
}
}
int main() {
  Temporary temporary;
  try {
    const auto path = temporary.path / "key.bin";
    const std::string password = "Clé locale 密钥";
    Key first; first.fill(17);
    Key next; next.fill(23);
    const auto envelope = protect_secret_key(SecretKey::from_bytes(first),bytes(password));
    atomic_write(path,envelope,false);
    environment("SYRNCK_KEY",{}); environment("SYRNCK_KEY_FILE",utf8(path)); environment("SYRNCK_KEY_PASSWORD",password);
    require(cli::encryption_key_environment() == first,"provider unwrapped wrong key");
    const auto before = std::chrono::steady_clock::now();
    for(unsigned at=0;at<32;++at)require(cli::encryption_key_environment() == first,"cached provider changed key");
    std::cout << Json{{"cached_opens",32},{"elapsed_us",std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-before).count()}}.dump() << '\n';
    environment("SYRNCK_KEY_PASSWORD","wrong");
    fails([&]{cli::encryption_key_environment();});
    environment("SYRNCK_KEY_PASSWORD",password);
    require(cli::encryption_key_environment() == first,"failed authentication destroyed valid cache");
    const auto stamp=fs::last_write_time(path);
    const auto changed=protect_secret_key(SecretKey::from_bytes(next),bytes(password));
    atomic_write(path,changed,true); fs::last_write_time(path,stamp);
    require(cli::encryption_key_environment() == next,"cache trusted unchanged size/time over changed envelope bytes");
    auto damaged=changed;damaged[88]^=1;atomic_write(path,damaged,true);
    fails([&]{cli::encryption_key_environment();});
    atomic_write(path,changed,true);
    require(cli::encryption_key_environment() == next,"restored envelope failed");
    environment("SYRNCK_KEY",hex(first));
    fails([&]{cli::encryption_key_environment();});
    environment("SYRNCK_KEY_FILE",{});
    require(cli::encryption_key_environment() == first,"raw provider compatibility failed");
    environment("SYRNCK_KEY",{});
    require(!cli::encryption_key_environment(),"missing provider became a key");
    std::cout<<"Native key-file provider cache, changed credentials and content refresh verified\n";
  } catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}
}
