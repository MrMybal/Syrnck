#include <syrnck/i18n.hpp>
#include <syrnck/core.hpp>
#include <algorithm>

namespace syrnck {
namespace {
struct Message {
  std::string_view english, french;
  bool prefix;
};
constexpr Message messages[] = {
#define SYRNCK_MESSAGE(en, fr) {en, fr, false},
#define SYRNCK_PREFIX(en, fr) {en, fr, true},
#include "messages.inc"
#undef SYRNCK_MESSAGE
#undef SYRNCK_PREFIX
};
} // namespace
Language parse_language(std::string_view code) {
  auto normalized = std::string(code);
  std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
    return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
  });
  if (normalized == "en" || normalized.starts_with("en-") || normalized.starts_with("en_"))
    return Language::english;
  if (normalized == "fr" || normalized.starts_with("fr-") || normalized.starts_with("fr_"))
    return Language::french;
  throw Error("unsupported language; use en or fr");
}
std::string_view language_code(Language language) {
  return language == Language::french ? "fr" : "en";
}
std::string localize(std::string_view english, Language language) {
  if (language == Language::french)
    for (const auto &message : messages) {
      if (english == message.english)
        return std::string(message.french);
      if (message.prefix && english.starts_with(message.english))
        return std::string(message.french) + std::string(english.substr(message.english.size()));
    }
  return std::string(english);
}
} // namespace syrnck
