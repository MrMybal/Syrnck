---
id: localization
title: Language and localization policy
description: Language and localization policy guide.
section: Getting started
order: 170
revision: 1
---

# Language and localization policy {#language-and-localization-policy}

English is Syrnck's source language and default interface language. French is the second built-in language. New interfaces must support per-session language selection and English fallback from their first implementation.

## Command-line selection {#command-line-selection}

```text
syrnck --help
syrnck --lang fr --help
syrnck --lang en --store data fsck
```

`--lang` takes precedence over `SYRNCK_LANG`. If neither is set, English is used regardless of `LANG`, `LC_ALL`, or Windows regional settings. `en` and `fr` are supported, including regional forms such as `en-US` and `fr_FR`. Unsupported explicit values fail with an error. Output uses UTF-8.

The CLI translates help, status text, its server's listening banner, and diagnostics owned by Syrnck. A complete catalog is compiled into the library, so an installed executable does not need external locale files. Translation introduces no global mutable language or process-locale change.

## Library and protocol boundaries {#library-and-protocol-boundaries}

```cpp
#include <syrnck/i18n.hpp>

auto language = syrnck::parse_language("fr");
auto display = syrnck::localize("object hash mismatch", language);
```

The core keeps canonical English exception text. A caller translates it for display with `localize`; another caller can select a different language concurrently. The default argument is `Language::english`. Unknown operating-system, standard-library, or dependency messages retain their original text.

Do not translate stored bytes, paths, user-authored messages, command/option names, reference names, JSON keys, object kinds, IDs, hashes, Git pointers, or protocol fields. C and Go ABI diagnostics also retain their canonical English form; language belongs to their application's presentation layer. HTTP error bodies remain canonical diagnostics independent of the server console's selected language. These boundaries keep automation, content hashes, and peers interoperable.

## Documentation languages {#maintaining-translations}

The local manual contains complete English and French editions with common page and anchor IDs. The CLI passes its selected language to contextual help. The reader can change language independently; unavailable routes fall back to English. Each translation records the English revision it covers. Developer maintenance instructions are kept separately from this manual.
