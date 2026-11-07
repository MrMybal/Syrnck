# Syrnck

[English](README.md) · [Français](README.fr.md)

**A C++20 block storage and revision engine for large assets.**

Syrnck splits files into reusable, content-addressed blocks. Complete manifests describe each version, so reconstruction never needs a chain of binary deltas. Git integration keeps ordinary Git history for code and stores selected assets as pointers. Standalone history uses the same blocks for commits, staging, branches, tags, recoverable checkout, merge, rebase, stash and inspection.

The engine also provides verified range reads, reuse plans, indexed packs, backups, retention, an authenticated block server and a resumable one-shot transfer agent. C++/C libraries, a Go binding and an optional regular-file Syncthing BEP adapter are available. No Unreal runtime is required.

**Experimental formats.** Standalone mode does not implement every Git feature. End-to-end encryption, a complete resident synchronization service and complete Syncthing daemon/folder interoperability remain unfinished. At-rest encryption does not hide data from an authorized server. See the manual’s current limits.

<a id="languages"></a>

## Local manual

The full English/French CyDoc manual is included in `Documentation/` and beside the installed executable. It opens directly through file:// without a server, CDN or Internet connection. Keep the complete folder together.

```text
syrnck help
syrnck help native tags
syrnck --lang fr help native-checkout
syrnck help packs --section memoire-du-gc --print-url
syrnck --help
```

You can also open [Documentation.html](Documentation/Documentation.html) in a browser from a local checkout or installation. On GitHub, browse the [English source](.CyDocBuild/manual/en/overview.md) or [French source](.CyDocBuild/manual/fr/overview.md); GitHub does not execute the reader. Search, navigation, code references and section links work locally. English is default; `--lang` overrides `SYRNCK_LANG`. Commands, IDs, JSON and wire formats keep their canonical spelling.

<a id="build-and-test"></a>

## Build, test and install

Requirements: CMake 3.25+, C++20 and C compilers, Ninja, Python 3.9+, and Node.js 22+/npm for manual generation. Windows uses Visual Studio 2022 C++ tools and a Windows SDK. Linux/macOS network builds require OpenSSL **3.x** development libraries. The helper discovers MSVC/CMake/Ninja on Windows.

```text
python scripts/build.py configure
python scripts/build.py build
python scripts/build.py test
cmake --install build --config Release --prefix dist
```

The CLI is `build/Release/syrnck.exe` on Windows and `build/syrnck` on Unix. Installation puts it in `dist/bin` with `Documentation/`, public headers, libraries and license notices in the normal installation directories. CI packages this installed layout. CMake presets `windows` and `unix` are available in an initialized compiler environment.

Small native dependencies and the pinned CyDoc package are included. Initial configuration downloads hash-verified Zstd/libcurl archives and npm’s locked build dependencies unless cached. macOS Homebrew users need `ninja` and `openssl@3`; select its directory with `-DOPENSSL_ROOT_DIR` if necessary.

For embedding use `add_subdirectory`, [core.hpp](include/syrnck/core.hpp) or the [C ABI](include/syrnck.h). Set `SYRNCK_BUILD_CLI=OFF`, `SYRNCK_BUILD_NETWORK=OFF`, `SYRNCK_BUILD_TESTS=OFF` and `SYRNCK_BUILD_BENCHMARKS=OFF` for a minimal build. Documentation then defaults off; `SYRNCK_BUILD_DOCUMENTATION=OFF` explicitly disables it. Installed help requires neither Node.js nor Python.

Regenerate the manual separately:

```text
npm --prefix .CyDocBuild ci --no-audit --no-fund --cache .CyDocBuild/.cache/npm
npm --prefix .CyDocBuild run build
npm --prefix .CyDocBuild run check
npm --prefix .CyDocBuild test
```

Authoring, translations and browser checks: [.CyDocBuild/README.md](.CyDocBuild/README.md).

<a id="quick-start"></a>

## Start a project

With the binary on PATH, run from a project directory:

```text
syrnck init
syrnck put Content/Map.uasset
syrnck native commit --message "Initial project snapshot"
syrnck refs
syrnck fsck
syrnck gc
```

`put` pins its manifest and reports reuse. `get MANIFEST NEW_FILE` reconstructs without overwriting. GC previews by default; `--delete` physically collects unreachable objects. Keep branches, archives, pins and verified backups for versions that must remain reconstructible. Start with `syrnck help usage`, then `help git-assets` or `help native-history`.

## Development and license

Local execution is Windows x64. CI is configured for Windows, Linux, macOS and Linux sanitizers; configuration alone does not establish remote results. Developer measurements and validation histories are outside the human manual under [.CyDocBuild/internal/records](.CyDocBuild/internal/records).

Before committing or publishing, run `python scripts/scan_secrets.py --gitleaks PATH_TO_GITLEAKS`; working-tree, staged and history modes are supported. Keep reports private under `.scratch`.

Original code is [MIT](LICENSE). Dependencies keep their own [licenses and notices](THIRD_PARTY.md). Optional Syncthing remains an external MPL-2.0 dependency. [Lore](https://github.com/EpicGames/lore) and [Syncthing](https://github.com/syncthing/syncthing) inspire the design; Syrnck’s native object and network formats are distinct.
