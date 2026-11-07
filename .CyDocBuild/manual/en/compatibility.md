---
id: compatibility
title: Compatibility and C++20 migration
description: Compatibility and C++20 migration
section: Getting started
order: 20
revision: 1
---

## Preserved formats {#formats}

The engine, CLI, server, agent, Git filters and C ABI are C++20/CMake. Native C dependencies require no Rust compiler/runtime. Logical v1 typed BLAKE3 IDs, canonical manifests/revisions, FastCDC 2020 boundaries (fastcdc 5.0.0, normalization 1, seed 0), SYR1/Zstd/XChaCha20-Poly1305 envelopes, physical v1 configuration/refs, Git pointers and HTTP v1 remain the compatibility baseline. See [format reference](doc:protocol).

New physical pack formats require supporting readers; explicit packing may select v2/v3, opening alone never converts. V3 stays v3 even after reducing pack count. Paged index access preserves existing binary bytes/hashes and adds no disk fields. Back up before enabling features unsupported by old binaries.

## Explicit differences and distribution {#adaptations}

Unicode normalization/casefold via utf8proc may reject equivalent names accepted by simpler lowercase checks. Ignore rules support nested glob/negation rules with documented scanner limits. C exceptions never cross the ABI: status 2 now means unknown C++ exception, and supported cancellable calls add status 3. ABI 1 gains symbols; pair the binding with matching rebuilt headers/library.

HTTPS uses libcurl with Schannel on Windows and OpenSSL 3 on Unix; redirects are disabled. The HTTP service is loopback-only. Binaries/libraries must be built for each OS/architecture. Static C++ libraries require their dependencies at final link. Define SYRNCK_STATIC for the static C ABI. See [integrations](doc:integrations), [Go](doc:go-binding), [licenses](doc:licenses) and the repository build instructions.
