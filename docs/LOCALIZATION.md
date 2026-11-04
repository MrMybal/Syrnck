# Language and localization policy

English is Syrnck's source language and default interface language. French is the second built-in language. New interfaces must support per-session language selection and English fallback from their first implementation.

## Command-line selection

```text
syrnck --help
syrnck --lang fr --help
syrnck --lang en --store data fsck
```

`--lang` takes precedence over `SYRNCK_LANG`. If neither is set, English is used regardless of `LANG`, `LC_ALL`, or Windows regional settings. `en` and `fr` are supported, including regional forms such as `en-US` and `fr_FR`. Unsupported explicit values fail with an error. Output uses UTF-8.

The CLI translates help, status text, its server's listening banner, and diagnostics owned by Syrnck. A complete catalog is compiled into the library, so an installed executable does not need external locale files. Translation introduces no global mutable language or process-locale change.

## Library and protocol boundaries

```cpp
#include <syrnck/i18n.hpp>

auto language = syrnck::parse_language("fr");
auto display = syrnck::localize("object hash mismatch", language);
```

The core keeps canonical English exception text. A caller translates it for display with `localize`; another caller can select a different language concurrently. The default argument is `Language::english`. Unknown operating-system, standard-library, or dependency messages retain their original text.

Do not translate stored bytes, paths, user-authored messages, command/option names, reference names, JSON keys, object kinds, IDs, hashes, Git pointers, or protocol fields. C and Go ABI diagnostics also retain their canonical English form; language belongs to their application's presentation layer. HTTP error bodies remain canonical diagnostics independent of the server console's selected language. These boundaries keep automation, content hashes, and peers interoperable.

## Maintaining translations

Owned messages are registered in `src/messages.inc` as English/French pairs. `SYRNCK_MESSAGE` matches an entire message. `SYRNCK_PREFIX` is reserved for diagnostics whose suffix is runtime data, such as an object ID or file path. It translates only the known prefix and copies the suffix unchanged. Do not implement general substring replacement on arbitrary exception text.

Add both languages whenever adding an owned diagnostic. New CLI help entries must appear in both help texts. The native catalog test checks every pair, fallback, dynamic suffixes, and caller isolation. The CLI tests check English defaults, French selection, precedence, invalid inputs, file reconstruction, stable JSON, and byte-identical Git filter output. A source-coverage check fails if a diagnostic literal passed to `require` or an owned exception constructor has no catalog entry.

Developer-facing code, public API names, comments, and new primary documentation should use English. `README.md` is the primary English entry point, with `README.fr.md` as its French counterpart. Older design notes are currently in French and are labeled as such in the README.
