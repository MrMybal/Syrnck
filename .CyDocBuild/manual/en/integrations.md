---
id: integrations
title: Library integrations
description: Library integrations
section: API
order: 700
revision: 2
---

## C++20 {#cpp}

Install the matching build with `cmake --install build --config Release --prefix dist`.
Point a separate application's `CMAKE_PREFIX_PATH` at that installation:

```cmake
cmake_minimum_required(VERSION 3.25)
project(MyApplication LANGUAGES C CXX)
find_package(Syrnck 0.1 CONFIG REQUIRED COMPONENTS Core)
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE Syrnck::core)
```

`cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/dist` locates the exported
package. It carries the distributed headers, C++20 requirement and static native
link dependencies. The installation can be moved as a whole; keep its `bin`,
`lib`, `include`, manual and license directories together. Do not copy a single
archive and guess its dependencies. Source builds expose the same public aliases.

| Target | Component | Purpose |
| --- | --- | --- |
| `Syrnck::core` | `Core` | Storage and local revisions/workspaces |
| `Syrnck::git` | `Git` | Git filters, catalogs and history integration |
| `Syrnck::net` / `Syrnck::git_net` | `Network` | HTTP transfers/server and Git catalog transfers |
| `Syrnck::ffi` / `Syrnck::ffi_static` | `C` | Shared/static C ABI |

Add required components to `find_package`; requesting `Network` from a core-only
installation fails clearly. Network-enabled Unix SDKs require a discoverable
OpenSSL 3 development package; CMake also resolves system threads. Bundled
BLAKE3, Zstandard, Monocypher, utf8proc and curl archives are installed with the
SDK. Targets for these dependencies are implementation details. A core-only
build (`SYRNCK_BUILD_NETWORK=OFF`, `SYRNCK_BUILD_CLI=OFF`) does not require
OpenSSL or curl.

Build a separate SDK per OS, architecture, compiler/standard-library ABI and
runtime/configuration. Match Release/Debug and the MSVC runtime; this package
does not promise binary compatibility across toolchains. `Syrnck::ffi_static`
propagates `SYRNCK_STATIC` and needs C++ linker support even when the entry point
is C. The shared C ABI is the boundary for consumers using another C++ ABI;
distribute its runtime library beside the application or configure the platform
loader. CMake import libraries do not deploy a Windows DLL automatically.

`ctest --test-dir build -C Release -R '^sdk_distribution$' --output-on-failure`
installs into a temporary prefix, relocates it, then compiles and runs independent
C++, shared/static C and optional network consumers. It verifies an encrypted
Store round trip and that exported targets retain no checkout/build/original
installation paths. Cross-platform results depend on actually running that gate.

The [Store API](doc:api-store) documents lock ownership, object verification and publication. [Progress](doc:operations), [backups](doc:backups), [text differences](doc:text-diff) and [workspace observation](doc:native-watch) describe operation-specific contracts. This reference uses explicit annotations from public distributed headers; it does not infer overload semantics or distribute implementation source listings.

## C and Go {#c-go}

`syrnck_ffi` exports the C ABI; `syrnck_ffi_static` uses SYRNCK_STATIC. Keep matching binary/header versions and never pass C++ objects across the boundary. Follow [C ownership/error rules](doc:api-c-abi), [Go integration](doc:go-binding) and [embedded workspace sessions](doc:workspace-abi). Windows uses dynamic DLL calls; Unix Go builds use cgo. Serialize handle operations, and capture native errors on the calling OS thread.

The separate [Syncthing adapter](doc:syncthing) implements regular-file block transfers, retained causal catalogs/tombstones and a resident folder controller against its pinned reference dependency. Devices are explicitly paired; discovery and directory/symlink metadata remain separate work.

## Platforms and interoperability {#platforms}

Build separate binaries for Windows, Linux and macOS and each architecture. The source supports MSVC and GCC/Clang with C++20. Native dependencies include BLAKE3, Zstandard, Monocypher, utf8proc, JSON and the optional networking libraries. [Notices](doc:licenses) travel with distribution.

For a minimal embedded build, disable CLI, network, benchmarks and tests; documentation generation can also be disabled explicitly. No Unreal headers or Unreal runtime are required by the engine. A future engine plugin is separate work. Lore and Syncthing designs inform some choices, but native Syrnck object/wire formats are distinct. BEP compatibility belongs to the explicit adapter, not to FastCDC or BLAKE3 objects.
