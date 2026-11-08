---
id: protocol
title: Formats and protocol
description: Formats and protocol guide.
section: Storage
order: 10
parent: storage
revision: 2
---

# Experimental formats and protocol {#formats-et-protocole-experimentaux}

Formats are versioned initial contracts, not a permanent compatibility promise. Object format, product and C ABI versions are independent.

## Identifiers and objects {#identifiants-et-objets}

The ID is BLAKE3 over the domain-separated byte sequence below. Allowed types: `chunk`, `manifest`, `revision`; IDs have exactly 64 lowercase hexadecimal characters. The full key is `{kind,id}`, separating identical bytes of different kinds.

Chunks contain 1 byte..16 MiB; an empty file has zero blocks and the ordinary BLAKE3 hash of the empty stream. All uncompressed objects, including metadata, are limited to 16 MiB.

Manifests are compact UTF-8 JSON without trailing newline, in order `version,size,file_hash,chunking,blocks`; each block has `id,size`. CDC config: `algorithm: fast-cdc2020,min,avg,max`; fixed: `algorithm: fixed,size`. The serialized spelling is authoritative (`inspect manifest`). C++ matches fastcdc 5.0.0 `v2020::StreamCDC::new`, normalization 1/seed 0. Changing boundaries requires a new algorithm/format name.

`file_hash` is ordinary BLAKE3 of complete file bytes without domain. Block sizes sum to `size`; reconstruction checks actual sizes and complete hash. Ingest workers/budget do not affect serialized boundaries/order/IDs. Flat manifest size is checked during reading before publication. Peak in-flight statistics are not object fields.

Revisions use compact JSON ordered `version,parents,message,timestamp,files`. UTF-8 path strings sort `files`; entries have `manifest,executable`. String escaping follows v1 canonical output from `nlohmann::ordered_json`; differing reserialization and unknown fields are refused. Independent writers need reference encoding vectors.

```text
ASCII("syrnck-object-v1") || 0x00 || ASCII(type) || 0x00 || contenu_non_compressé
```


## Local storage {#stockage-local}

Binary envelopes begin `SYR1`, then a mode byte:0 for Zstd;1 for a 24-byte nonce followed by XChaCha20-Poly1305 encrypted Zstd including tag. Associated ASCII is `syrnck-envelope-v1:<type>:<id>`, so moving ciphertext to another key fails authentication.

Without packs, config has `version:1,key_check`; encrypted key_check is keyed BLAKE3 of `syrnck-store-key-check-v1` with the Store key, otherwise null. Ref names/IDs remain visible in `refs.json`.

The read cache adds no files or fields. It retains nothing and cannot hide missing objects. Entries come from validation in the current handle and are reused only after cryptographic comparison of reread envelopes. Read/decode counts may change, never logical bytes or complete-file validation rules.

```text
<store>/config.json
<store>/refs.json
<store>/store.lock
<store>/objects/<type>/<deux-premiers-caractères>/<id>
```


### Physical v2 packs {#stockage-physique-v2-avec-packs}

An explicit pack may publish exactly `version:2,key_check,pack`. `pack` is null or a descriptor with exactly `name` (32 lowercase hex), positive `objects`, pack `bytes`, `index_hash` (ordinary BLAKE3 of the whole index). Config is the atomic publication pointer; logical objects/refs/envelopes/network remain v1. V1-only readers must refuse v2.

Files are `packs/pack-<name>.syp/.syi`. Pack header: eight ASCII bytes `SYRPACK1`, then unchanged concatenated `SYR1` envelopes with no holes/unindexed data. Binary integers are unsigned little-endian.

Index header: eight ASCII bytes `SYRIDX01`, then 8-byte object count. Each 45-byte record:1-byte kind (0 chunk,1 manifest,2 revision),32-byte binary hash,8-byte absolute envelope offset,4-byte envelope length. Strict kind/ID order, no duplicates; ranges contiguous from offset 8 to `bytes`. Envelope 5 bytes..17 MiB; at most 1,048,576 entries and pack size `INT64_MAX`. Index length exactly `16+45*objects`, hash equal config.

Opening/audits reload and verify indexes. Read sessions validate pack identity/size/header and share a handle during reconstruction/audit, closing afterward; isolated reads use their own session. Every envelope is reread, authenticated/decompressed/ID/format checked even on cache hits. Index hash detects changes but does not sign/authenticate the catalog. Types/IDs/positions/sizes remain visible when encrypted.

Writes remain loose; reads prefer loose duplicates, logical inventory counts once, fsck checks both. Inactive packs retain nothing. See [publication/recovery](doc:packs).

### Physical v3 pack catalog {#stockage-physique-v3-catalogue-de-packs}

V3 config has exactly `version:3,key_check,packs`, an array of v2 descriptors. Empty or single-pack arrays stay v3; multiple packs require v3 and older readers must refuse it. Binary pack/index encodings are unchanged.

Names must be distinct and no typed key may occur in two active packs even with identical bytes. Separate index validation plus merged overlapping traversals detect duplicates. Limits:1,024 packs,4,194,304 total objects,1,048,576 per index. Declared sizes are checked before reading indexes, followed by ranges/content validation.

Ordinary maintenance appends; compact may replace all, GC only some. All files are written/verified before one atomic config replacement. Unpublished batches are inactive and contribute no inventory. Reconstruction/audit reuse at most eight LRU pack handles, closed at completion, separately from page/decoded budgets. Envelope validation is unchanged.

### Paged access without a format change {#consultation-paginee-sans-changement-de-format}

The flat `SYRIDX01` and complete hash remain. Opening streams 256-record pages to construct fences/digests and a negative-key filter. Those and the cache exist only in RAM; no store descriptor/header/auxiliary file changes. New indexes stream the same binary encoding.

Candidate reread pages must match their validated digest before addresses are used. Hot pages/last address may remain an opened locked catalog snapshot, like the former RAM table. Audits reload and hash complete indexes independently. Actual envelope validation remains mandatory. See [budgets and guarantees](doc:indexes).

## Native workspace selection formats {#native-head-formats}

Named `workspace/HEAD` descriptors use canonical v1 JSON; detached descriptors
use v2 with `branch: null`, `type: "native-head"` and the canonical `directory`.
Private roots are `workspace/detached` (revision) and `workspace/staging`
(optional prepared revision). Checkout records involving a detached target use
v7 with `layout_version` 1..6; all named-only layouts keep their old versions.
Detached stash records use v2 with target `:HEAD`; named stash records stay v1.
These additions do not change immutable revision v1 or pack/ABI versions. Older
executors must reject unsupported records. See [detached HEAD](doc:native-detached).

## Git pointer {#pointeur-git}

All three lines terminate with LF. Encoding is canonical and limited to 1024 bytes; size must match the manifest. A malformed document beginning with the reserved header is an error. This is distinct from Git LFS pointers.

```text
version syrnck-pointer-v1
manifest blake3:<64 caractères hexadécimaux>
size <entier décimal>
```


## HTTP API {#api-http}

Every request carries `Authorization: Bearer <token>`. One Store is served, with legacy shared authority or configured [users/permissions](doc:server-access). Clients require HTTPS except loopback; the server listens on loopback for deployment behind a TLS proxy.

| Method/path | Contract |
| --- | --- |
| `POST /v1/missing` | JSON typed keys, absent subset, maximum 4096 |
| `GET /v1/objects/{kind}/{id}` | Logical bytes or negotiated Zstd frame, maximum 16 MiB logical |
| `PUT /v1/objects/{kind}/{id}` | Verify kind/hash/format;201 created,204 present |
| `GET /v1/refs` | Name-to-key JSON table |
| `PUT /v1/refs/{name}` | `{root:key,expected:key or null}`, verify complete dependencies |

Remote names use one URL component, local names may contain `/`. Expected null means absent, otherwise exact equality; conflict 409, missing dependency 404, invalid/corrupt object error. Request JSON field ordering is unrestricted; only immutable objects require canonical encoding. Objects may arrive before dependencies; a new version is visible only after verified ref publication. Existing-object PUT is idempotent. Redirects are disabled for authenticated requests.

This v1 is not BEP, Git LFS or Lore. `GET /v1/capabilities` negotiates [upload leases](doc:server-upload-leases), lifetime and receipts; protected publications add `upload:{id,fence}`. Legacy requests remain unless leases are required. [Read leases](doc:server-read-leases) advertise `read_leases:1,read_lease_seconds`, acquired/renewed via `PUT /v1/reads/ID`, inspected by GET and released via `POST /v1/reads/ID/release`, retaining an exact root before downloads.

[Upload inventories](doc:upload-inventories) advertise `upload_inventories:1`; lease creation adds `inventory:true`, pages `PUT /v1/uploads/ID/inventory/INDEX` declare keys/token before missing queries. Chained page count/final digest are in schema 2; schema 1 retains global protection. [Compression](doc:network-compression) advertises `object_zstd:1`; `Syrnck-Accept-Object-Encoding:zstd` and `Syrnck-Object-Encoding:zstd` negotiate one frame/object, legacy stays raw. Client validates size/window/integrity/format before import; JSON/CAS representation is unchanged. User/object quotas and server replication remain future work.

## C interface {#interface-c}

`include/syrnck.h` defines ABI1 (`syrnck_abi_version()`), independent of objectv1/product 0.1.0. Status 0 success,1 error,2 unknown C++ exception,3 cooperative cancellation for supported calls. No exception crosses C. Strings are NUL-terminated UTF-8; serialize handles including close. Input pointers are not retained.

`object_has` verifies presence and distinguishes corruption. `object_read` gives verified uncompressed bytes: null/zero-capacity query returns size, then provide enough caller buffer; insufficient capacity writes nothing. `closure_json` follows the same rule with NUL included in length. Import checks hash/format without requiring dependencies.

`ref_set` verifies the complete closure/file hashes then replaces a local pin without CAS; HTTP is distinct CAS. Unpublished imported objects are not protected after close: keep the Store open and serialize transfer/GC, then retain a root before closing.

`last_error` is thread-local, valid until a modifying call, bounded 4095 bytes to avoid allocating in error handling. `error_copy` copies without changing it, length includes NUL. Goroutine bindings must retrieve it on the same OS thread. Only allocated `manifest_json` strings use `string_free`; ordinary buffers belong to the caller. Additive range, comparison, file-verification and [workspace APIs](doc:workspace-abi) require matching rebuilt headers/library. See [C ownership reference](doc:api-c-abi).
