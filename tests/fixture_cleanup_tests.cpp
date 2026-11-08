#include "native_history_fixture.hpp"
#include "fixture_cleanup.hpp"
#include <iostream>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
using namespace native_test;
int main() {
  try {
    Temporary temporary;
    const auto empty = temporary.path / "empty", occupied = temporary.path / "occupied";
    fs::create_directories(empty); fs::create_directory(occupied);
    require(fixture_test::remove_empty_directory(empty), "empty fixture directory was not removed");
    require(!fixture_test::remove_empty_directory(empty), "absent fixture directory reported removal");
    atomic_write(occupied / "keep", bytes("must not delete a child"), false);
    bool refused = false;
    try { fixture_test::remove_empty_directory(occupied, std::chrono::milliseconds(40)); }
    catch (const fs::filesystem_error &) { refused = true; }
    require(refused && text(read_bytes(occupied / "keep")) == "must not delete a child",
        "fixture helper swallowed a persistent nonempty directory or removed a child");
#ifdef _WIN32
    const auto pending = temporary.path / "pending"; fs::create_directory(pending);
    const auto child = pending / "child";
    HANDLE file = CreateFileW(child.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW, 0, nullptr);
    require(file != INVALID_HANDLE_VALUE, "cannot create pending-deletion fixture");
    if (!DeleteFileW(child.c_str())) { CloseHandle(file); throw Error("cannot mark fixture child for deletion"); }
    std::jthread close_child([file] { std::this_thread::sleep_for(std::chrono::milliseconds(40)); CloseHandle(file); });
    require(fixture_test::remove_empty_directory(pending), "pending child prevented fixture cleanup");
    close_child.join();
#endif
    std::cout << "Fixture cleanup preserves children and bounds delayed Windows deletion\n";
    return 0;
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
