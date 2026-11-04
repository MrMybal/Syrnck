#pragma once
// Only the dedicated crash-test objects define this macro. Normal libraries
// and applications have neither checkpoint calls nor runtime fault controls.
#ifdef SYRNCK_TEST_IO_CHECKPOINTS
#include <filesystem>
#include <string_view>
namespace syrnck::testing {
void io_checkpoint(std::string_view phase, const std::filesystem::path &target);
}
#define SYRNCK_IO_CHECKPOINT(phase, target) ::syrnck::testing::io_checkpoint(phase, target)
#else
#define SYRNCK_IO_CHECKPOINT(phase, target) ((void)0)
#endif
