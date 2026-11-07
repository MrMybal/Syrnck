---
id: audit
title: Streaming integrity audits
description: Streaming integrity audits guide.
section: Storage
order: 40
parent: storage
revision: 1
---

# Streaming integrity audits {#streaming-integrity-audits}

`Store::fsck()` and `syrnck fsck` verify every logical object in the active store,
including unreachable revisions, manifests and chunks. This is a read-only audit;
retention and deletion remain the responsibility of `gc`.

After a known interrupted GC, dead loose metadata can temporarily outlive its
dead dependencies and fail this complete audit. Retained versions remain
reconstructible; see [resuming interrupted cleanup](doc:crash-recovery#interrupted-garbage-collection).

The audit reloads the configuration and validates the complete active pack
indexes. It bypasses decoded data and validation caches, checks references, then
enumerates revisions, manifests and chunks without constructing a complete
inventory of IDs. Pack page fences select only pages intersecting the requested
object kind. A page can contain several kinds; each record is filtered while its
original position in the pack is preserved.

Metadata comes before standalone chunks. A manifest's file hash is checked while
reading its blocks in order, so enumeration does not first decode every chunk
and then read those chunks again to hash its file. Shared blocks and repeated
positions are still read wherever required by a file's complete hash. This does
not promise one read per object for every dependency graph.

Every read still validates the envelope, authentication when encrypted, object ID
and object format. Missing dependencies and incorrect complete file hashes fail
the audit, including those found only in unreachable metadata. A logical object
present both loose and packed counts once, but both physical copies are checked.
An inactive pack is outside the active inventory and contributes no retention root.

## Memory and lifetime {#memory-and-lifetime}

Packed objects use one mark bit per index entry, rounded up separately for each
pack. For 1,048,576 entries in one pack, the useful bitmap is 131,072 bytes. The
page directory, negative-key filter and configurable index page cache have their
own costs, described in [paged indexes](doc:indexes).

Objects absent from the pack catalog require a compact set of typed IDs, including
all loose-only objects audited. References, the current dependency stack, decoded
objects, JSON and allocator overhead also use memory. Large individual manifests
and revisions remain materialized. Total engine memory is not globally bounded
independently of project size.

Each enumeration page owns its buffer, so nested lookups remain valid even when
the index page cache is disabled. Nested read sessions share bounded file handles;
their outermost return or exception closes the handles. The ordinary decoded-cache
policy is restored after success or failure. The store retains its exclusive lock
throughout the operation. Public `objects()` and `closure()` still return complete
collections. Packing/compaction now use [streaming plans](doc:compaction), while
their loose-only input keys still need sorting.

No storage, network, C ABI or logical object format changes are needed.

## Interpreting resource use {#verification-and-measurement}

For interpreting process memory and tuning cache budgets, see [performance and resource limits](doc:performance). Synthetic benchmark fixtures do not establish throughput or capacity for an Unreal project. An integrity audit is read-only and checks actual stored bytes, independently of cache statistics.

## Progress and cancellation {#progress-and-cancellation}

`fsck` accepts `OperationOptions` and the CLI supports `--progress` or
`--progress-json`. Counters track decoded payload reads without materializing
the inventory. Interrupted audits close their read sessions and restore normal
cache use; store contents and retention roots are unchanged. See the
[operation contract and limits](doc:operations).
