#pragma once
#include <string>
#include <string_view>

namespace syrnck {
// Select a language per interface/session; the storage engine never changes
// process locale. Object IDs, formats, protocol fields and Error::what() stay English.
enum class Language { english, french };
Language parse_language(std::string_view code);
std::string_view language_code(Language language);
// Translate owned messages at the presentation boundary. Unknown third-party
// diagnostics fall back to the original text; paths and user data are preserved.
std::string localize(std::string_view english, Language language = Language::english);
} // namespace syrnck
