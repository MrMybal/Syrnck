---
id: integrations
title: Library integrations
description: Library integrations
section: API
order: 700
revision: 1
---

## C++20 {#cpp}

Use the distributed headers and CMake targets. `syrnck_core` owns storage and local revision/workspace operations; `syrnck_git` adds Git integration; `syrnck_net` adds HTTP; `syrnck_git_net` connects Git catalogs to transfers. The `Syrnck::core` alias is available in the source build. Static C++ archives require their native dependencies to be linked as well.

The [Store API](doc:api-store) documents lock ownership, object verification and publication. [Progress](doc:operations), [backups](doc:backups), [text differences](doc:text-diff) and [workspace observation](doc:native-watch) describe operation-specific contracts. This reference uses explicit annotations from public distributed headers; it does not infer overload semantics or distribute implementation source listings.

## C and Go {#c-go}

`syrnck_ffi` exports the C ABI; `syrnck_ffi_static` uses SYRNCK_STATIC. Keep matching binary/header versions and never pass C++ objects across the boundary. Follow [C ownership/error rules](doc:api-c-abi), [Go integration](doc:go-binding) and [embedded workspace sessions](doc:workspace-abi). Windows uses dynamic DLL calls; Unix Go builds use cgo. Serialize handle operations, and capture native errors on the calling OS thread.

The separate [Syncthing adapter](doc:syncthing) implements the regular-file block-data subset against its pinned reference dependency. It does not manage device pairing, causal versions, deletions or a resident sync daemon.

## Platforms and interoperability {#platforms}

Build separate binaries for Windows, Linux and macOS and each architecture. The source supports MSVC and GCC/Clang with C++20. Native dependencies include BLAKE3, Zstandard, Monocypher, utf8proc, JSON and the optional networking libraries. [Notices](doc:licenses) travel with distribution.

For a minimal embedded build, disable CLI, network, benchmarks and tests; documentation generation can also be disabled explicitly. No Unreal headers or Unreal runtime are required by the engine. A future engine plugin is separate work. Lore and Syncthing designs inform some choices, but native Syrnck object/wire formats are distinct. BEP compatibility belongs to the explicit adapter, not to FastCDC or BLAKE3 objects.
