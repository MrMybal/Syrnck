#pragma once
#include <syrnck/core.hpp>
#include <syrnck/i18n.hpp>
namespace syrnck::cli {
std::string documentation_url(const fs::path &directory, Language, std::string topic,
                              const std::optional<std::string> &section = {});
fs::path documentation_directory(const std::optional<std::string> &override_directory = {});
void open_documentation(std::string_view url);
} // namespace syrnck::cli
