#include <syrnck/core.hpp>
#include <syrnck/i18n.hpp>
#include <future>
#include <iostream>
using namespace syrnck;
int main() {
  try {
    require(parse_language("en") == Language::english && parse_language("FR-fr") == Language::french &&
                parse_language("en_US.UTF-8") == Language::english,
            "language selection failed");
    bool failed = false;
    try {
      (void)parse_language("de");
    } catch (const Error &) {
      failed = true;
    }
    require(failed, "unsupported language accepted");
    require(localize("object hash mismatch") == "object hash mismatch", "default is not English");
    require(localize("opaque third-party data", Language::french) == "opaque third-party data",
            "unknown message did not fall back");
    Error original("object hash mismatch");
    require(localize(original.what(), Language::french) == "le hash de l'objet ne correspond pas" &&
                std::string_view(original.what()) == "object hash mismatch",
            "canonical error was changed");
    std::set<std::string_view> keys;
    std::size_t count = 0;
    auto check = [&](std::string_view english, std::string_view french, bool prefix) {
      require(keys.insert(english).second && !french.empty(), "duplicate or empty translation");
      require(localize(english, Language::english) == english &&
                  localize(english, Language::french) == french,
              "catalog lookup failed");
      if (prefix) {
        std::string suffix = "C:/données/a-b.bin: 0123456789abcdef";
        require(localize(std::string(english) + suffix, Language::french) == std::string(french) + suffix,
                "dynamic diagnostic changed user data");
      }
      ++count;
    };
#define SYRNCK_MESSAGE(en, fr) check(en, fr, false);
#define SYRNCK_PREFIX(en, fr) check(en, fr, true);
#include "messages.inc"
#undef SYRNCK_MESSAGE
#undef SYRNCK_PREFIX
    auto task = [](Language language) {
      for (std::size_t i = 0; i < 1000; ++i)
        require(localize("Error: ", language) == (language == Language::french ? "Erreur : " : "Error: "),
                "language leaked between callers");
    };
    auto a = std::async(std::launch::async, task, Language::english);
    auto b = std::async(std::launch::async, task, Language::french);
    a.get();
    b.get();
    std::cout << count
              << " bilingual messages verified; default, fallback, data and caller isolation passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
