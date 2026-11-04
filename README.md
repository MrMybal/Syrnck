# Syrnck

[English](README.md) · [Français](README.fr.md)

**A C++20 block storage and revision engine for large assets and differential file synchronization.**

Syrnck splits files into reusable, content-addressed blocks instead of storing a full copy after every change. Complete manifests describe each file version, so reconstruction does not depend on a chain of binary deltas.

Two modes share the same engine: **Git integration**, retaining Git's workflow for source code while storing assets as Syrnck pointers, and **standalone revisions**, building toward an independent version control system. The library is independent of Unreal Engine and can be embedded through C++ or its C ABI. A Go binding provides a foundation for future synchronization adapters.

**Status: working prototype with experimental formats.** The engine, Git filters, local revisions, server, and one-shot transfer agent run today. Standalone mode does not yet implement Git's full feature set. Unreal project benchmarks, an Unreal plugin, end-to-end encryption, and Syncthing protocol interoperability remain future work.

## Components

| Component | Available functionality |
| --- | --- |
| `syrnck_core` | FastCDC 2020 or fixed blocks, BLAKE3, Zstd, verified reconstruction, indexed packs, revisions, retention, and GC |
| `syrnck_git` | Clean/smudge filters; source stays in Git, assets use Syrnck pointers |
| `syrnck_net` | Authenticated HTTP v1 API, missing-object comparison, differential transfer, and compare-and-swap reference publication |
| `syrnck` | Cross-platform CLI, loopback server, and push/pull agent |
| `syrnck_ffi` / `syrnck_ffi_static` | Shared/static C ABI for blocks, verified import, dependencies, reconstruction, and pins |
| `bindings/go` | Go binding and resumable differential copying; Windows DLL / Unix cgo backends |

XChaCha20-Poly1305 protects objects **at rest**. An authorized server can read their contents. The HTTP server binds to loopback and is intended to sit behind an HTTPS proxy.

## Build and test

Requirements: CMake 3.25+, a C++20 compiler, a C compiler, and Ninja. On Windows, install the Visual Studio 2022 C++ tools and a Windows SDK. Linux/macOS also need OpenSSL **3.x** development libraries. Python 3.9+ runs the build helper and tests; the engine and CLI are native binaries.

```text
python scripts/build.py configure
python scripts/build.py build
python scripts/build.py test
```

The helper discovers MSVC/CMake/Ninja on Windows. In an initialized build environment, the presets can also be used directly:

```text
cmake --preset windows
cmake --build --preset windows
ctest --preset windows
```

Use the `unix` preset on Linux/macOS. Small dependencies are vendored; CMake downloads pinned Zstd and libcurl archives and verifies their SHA-256 hashes. See [third-party components and notices](THIRD_PARTY.md).

On macOS with Homebrew, install `ninja` and `openssl@3`. If multiple OpenSSL versions are installed, configure with `python scripts/build.py configure -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"`. The pinned curl requires OpenSSL 3.x; the network build rejects OpenSSL 4 instead of compiling against incompatible APIs.

The CLI is `build/Release/syrnck.exe` on Windows and `build/syrnck` with the Unix preset. Embed the C++ library through [core.hpp](include/syrnck/core.hpp), or use the [C ABI](include/syrnck.h). CMake targets are available through `add_subdirectory`.

## Quick start

With the binary directory on `PATH`, run these commands from a project directory:

```text
syrnck init
syrnck put Content/Map.uasset
syrnck native commit --message "Initial project snapshot"
syrnck refs
syrnck fsck
syrnck gc
```

`put` retains a file and prints its manifest ID and reuse statistics. `get MANIFEST NEW_FILE` reconstructs a file without overwriting an existing destination. `gc` is a dry run; `gc --delete` removes unreachable objects. Branches, revisions, archives, and pins retain their complete block dependencies, including shared blocks.

```text
syrnck put large-file.bin --workers 4 --buffer-mib 32
syrnck pack
syrnck pack --compact
syrnck --lang fr --help
```

Parallel compression/encryption is optional and uses a bounded queue; sequential ingestion remains the default. Indexed immutable packs reduce file counts. Ordinary packing appends new packs; explicit compaction regroups the store. GC keeps unaffected packs and rewrites only partially reachable packs.

The verified object cache defaults to 64 MiB and 8,192 fingerprints. Every hit still checks the freshly read envelope; `fsck` and `gc` bypass decoded validation caches. The independent index page cache defaults to 4 MiB (`--index-cache-mib`). GC enumerates objects and rewrites survivors in a stream, with one mark bit per packed object. Loose live keys and metadata still consume memory proportional to their size.

Packing explicitly enables physical storage v2 for a single pack, or v3 for multiple packs. New stores remain v1 until packing; logical IDs and encrypted envelopes stay unchanged. Older readers reject unsupported physical formats. See [pack publication and recovery](docs/PACKS.md), [paged indexes](docs/INDEXES.md), and [reproducible measurements](docs/PERFORMANCE.md), currently documented in French.

## Languages

**English is the default. French is the second built-in interface language.** Select `--lang en` / `--lang fr`, or set `SYRNCK_LANG=en` / `SYRNCK_LANG=fr`. The command-line flag takes precedence. Operating-system locale settings do not change the default.

Help, status messages, and owned diagnostics are localized at the interface boundary. Commands, option names, JSON keys, object IDs, Git pointers, and network formats are language independent. Core exceptions and C/Go diagnostics retain their canonical English text; C++ interfaces can use [i18n.hpp](include/syrnck/i18n.hpp) for presentation. Unknown operating-system/dependency diagnostics fall back to their original text. See [localization policy](docs/LOCALIZATION.md).

## Git and synchronization

In an existing Git repository, run `syrnck git install`, then add the appropriate rules from [examples/unreal.gitattributes](examples/unreal.gitattributes). **A regular `git push` does not yet upload Syrnck asset blocks.** Transfers currently use the explicit `agent push` / `agent pull` commands. Automatic pre-push transfer, checkout hydration, and a long-running synchronization watcher are planned.

The project takes inspiration from [Lore](https://github.com/EpicGames/lore) for chunked storage and [Syncthing](https://github.com/syncthing/syncthing) for differential synchronization. No implementation code from either project was copied into this engine. Syncthing interoperability is not currently implemented.

## Validation and repository publishing

Local builds and tests have been run on Windows x64 with MSVC. CI is configured for Windows, Linux, macOS, and Linux sanitizers; configuration alone does not establish that those remote runs have passed. The C++ implementation needs no Rust compiler or runtime. Compatibility fixtures exercise the original v1 Rust format, including encrypted stores.

Run [Gitleaks](https://github.com/gitleaks/gitleaks) before committing or uploading:

```text
python scripts/scan_secrets.py --gitleaks PATH_TO_GITLEAKS
```

The script scans a snapshot of the publishable working tree, including vendored dependencies, while leaving build outputs, local tools, stores, and scan reports out of Git. [GitHub publishing instructions](docs/PUBLISHING.md) describe the first import into `MrMybal/Syrnck`, authentication, staged/history scans, and CI. This Git source upload is independent of Syrnck's experimental asset transfer protocol.

## Further reading

- [Localization and contribution rules](docs/LOCALIZATION.md) — English
- [GitHub publishing and repository description](docs/PUBLISHING.md) — English
- [Usage](docs/USAGE.md), [architecture](docs/ARCHITECTURE.md), [formats](docs/PROTOCOL.md), [roadmap](docs/ROADMAP.md), [performance](docs/PERFORMANCE.md), and [migration](docs/MIGRATION_CPP.md) — French
- [Go binding](bindings/go/README.md) — French

The project has not selected its own distribution license yet. Dependency notices are listed in [THIRD_PARTY.md](THIRD_PARTY.md).
