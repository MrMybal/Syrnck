---
id: network-compression
title: Negotiated object compression
description: Negotiated object compression guide.
section: Transfers
order: 430
parent: transfers
revision: 1
---

# Negotiated object compression {#negotiated-object-compression}

Object uploads and downloads use Zstandard by default when the server advertises
support. Compression changes the wire representation only. BLAKE3 object IDs,
full manifests, at-rest encryption/compression, local session records and final
reference publication retain their existing contracts.

```text
syrnck agent push https://blocks.example.invalid files/MANIFEST asset --requests 4 --compression auto
syrnck agent pull https://blocks.example.invalid asset received --compression zstd --transfer-buffer-mib 17
syrnck agent resume https://blocks.example.invalid SESSION --compression off
syrnck serve --no-compression
```

These commands require the usual initialized store, token and HTTPS setup.
`auto` is the default; `off` sends and requests logical bytes directly; `zstd`
requires support when an object actually needs transferring. Even in `zstd`
mode, small or incompressible objects can travel raw. The server can disable
the extension with `--no-compression` (`ServeOptions::object_compression=false`).
In C++, append `TransferOptions::compression` with
`net::ObjectCompression::{automatic, disabled, zstd}`.

## Negotiation and representation {#negotiation-and-representation}

`GET /v1/capabilities` adds the integer `object_zstd`, either **0** or **1**.
The client negotiates lazily before the first missing object, once per operation.
An absent field or a 404 capability endpoint permits identity fallback in auto
mode. Invalid types/versions, authentication errors and other request failures
fail the operation. A fully reused transfer or completed session needs no
compression negotiation. Resumption can select a different mode without
changing the session identity or its expected reference.

| Message | Representation |
| --- | --- |
| Object PUT with `Syrnck-Object-Encoding: zstd` | One Zstandard frame |
| Object PUT without that header | Original logical bytes |
| Object GET with `Syrnck-Accept-Object-Encoding: zstd` | Client accepts a frame or logical bytes |
| Object GET without that header | Logical bytes |
| Object response with `Syrnck-Object-Encoding: zstd` | One Zstandard frame |
| Object response without that header | Logical bytes |

Object bodies use `application/octet-stream`. Compression applies to chunks,
manifests and revisions. JSON control requests and object acknowledgements stay
uncompressed. GET replies include `Vary: Syrnck-Accept-Object-Encoding`.

These are **Syrnck protocol headers**, deliberately separate from HTTP
`Content-Encoding`. This keeps encoded-byte accounting and bounded decoding
inside the engine instead of an HTTP library's automatic decoder. The client
requests HTTP identity and disables automatic HTTP content decoding. Unexpected
HTTP content codings, duplicate object encoding headers, unsupported object
codings or encoding fields in trailers fail. A response cannot introduce
compressed objects unless the client accepted them. The server rejects encoded
PUTs when disabled, unknown/duplicate encodings and encoded control requests
with 415. The custom header value is exactly `zstd`.

Custom coding rejection happens after reading the bounded request body, so
an unread body cannot reset the connection and hide its 415 response. Unsupported
HTTP `Content-Encoding` can be rejected earlier by the HTTP library; clients
must also tolerate a connection failure on such unsupported requests.

HTTPS proxies must preserve both custom headers, `Vary` and the object bytes.
A proxy which strips the representation header causes verification failure;
clients do not reinterpret opaque failures as a reason to retry without
compression. Old clients still receive logical bodies from new servers.

## Bounds and verification {#bounds-and-verification}

Both encoded and logical object bodies are limited to **16 MiB**. A supported
frame has a known content size, no dictionary, and a window no larger than
16 MiB. Skippable frames, concatenated frames, trailing bytes, truncated frames,
unknown-size frames and oversized declarations are rejected. The frame header
is validated before allocating the decoded output; the streaming decoder also
caps its window. Frames are not themselves an authentication boundary: the Store
still verifies the logical object's type, BLAKE3 ID and schema before import.
Final publication still verifies the complete dependency closure and file hashes.

Encoding uses Zstandard level **1**, attempts objects of at least **1 KiB**, and
requires a saving of at least **64 bytes**. If the candidate cannot fit within
the currently available staging credit, or compression cannot achieve that
saving, the original body is sent. No speculative larger encoded buffer is
allocated. Compression and Store work remain on the coordinator; cancellation
is checked before and after compression and between decoding steps.

The [object buffer budget](doc:parallel-transfers) includes the original upload,
the candidate encoded vector while both coexist, and the reserved acknowledgement.
The peak counter includes this preparation stage even if the candidate is
discarded. After encoding, only the selected upload vector remains charged.
Downloads keep their 16 MiB reservation and decode directly into their response
vector without retaining an entire encoded copy. The 17 MiB CLI minimum still
works. Codec contexts, transport buffers, Store caches and metadata remain
outside this body-credit budget; this is not a process RSS limit.

## Traffic, retries and recovery {#traffic-retries-and-recovery}

`transferred_bytes` continues to count successful **logical** bytes once.
Two additional result fields count HTTP object-body bytes in each direction:

| Field | Meaning |
| --- | --- |
| `object_upload_bytes` | Uploaded object request bodies across all attempts |
| `object_download_bytes` | Downloaded object response bodies across all attempts, including PUT acknowledgements |

These fields exclude HTTP headers, TLS framing and capability/presence/lease/ref
traffic. Partial and repeated attempts count; a fully reused transfer reports
zero. [Rate targets](doc:transfer-policy) and transfer progress also use encoded
body bytes. They must not be derived from `transferred_bytes` after compression.
Error/cancellation exits do not return a successful statistics object.

Idempotent upload retries retain the same selected encoded body. Interrupted
downloads restart the object with a fresh decoder; partial decoded bytes never
enter the Store. Compression/header/format errors are not retried. Previously
verified objects remain reusable through the ordinary persistent session and
lease mechanisms. The final reference CAS is still never retried automatically.

## Current limits {#validation-and-remaining-work}

Compression context reuse, CPU budgets, real asset corpus measurements, cross-platform execution and presence caching remain further work. Wire savings depend on the data; compression does not establish a universal throughput gain.
