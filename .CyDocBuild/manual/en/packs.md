---
id: packs
title: Packs and compaction
description: Packs and compaction guide.
section: Storage
order: 10
parent: storage
revision: 1
---

# Packs and compaction {#packs-et-compaction}

`syrnck pack` groups loose objects into indexed immutable packs while preserving IDs, `SYR1` envelopes, encryption nonces, manifests and references. Ordinary maintenance adds packs; `pack --compact` explicitly regroups all objects. Neither removes logical versions. C++: `Store::pack(PackOptions)`.

```text
syrnck --store donnees pack
syrnck --store donnees pack --compact
syrnck --store donnees pack --max-mib 512 --max-objects 65536
syrnck --store donnees gc
syrnck --store donnees gc --delete
```


## Activation and compatibility {#activation-et-compatibilite}

New stores remain physical v1 (one file/object). The first explicit pack publishes v2 when one pack suffices, v3 for several; adding a second pack to v2 selects v3. Logical objects/network remain v1. The engine reads all three; older binaries refuse unknown physical versions. Once v3, it stays v3 even with one/no packs. Opening never converts; no downgrade command exists.

New writes remain loose. Reading combines loose/active packs; loose+packed duplicates count once and ordinary reads prefer loose. `fsck` checks both physical copies. A key in two active packs is invalid. CLI, filters, server, C and Go read via this core; peers exchange verified logical objects independently of layouts/keys. Maintenance is CLI/C++ only.

## Incremental and full packing {#regroupement-incremental-et-complet}

Default pack selects loose-only keys and appends new packs. Existing names/bytes/timestamps remain; redundant loose copies are removed after verification. With no new objects/copies, v2/v3 does not rewrite pack/index/config, though inactive leftovers may be cleaned. `--compact` regroups everything to reduce pack count while respecting limits; a single pack is not guaranteed.

`--max-mib`: 18..4096 MiB, default 512; `--max-objects`: 1..1,048,576, default 65,536. The byte budget includes the eight-byte header; whole envelopes up to 17 MiB must fit. C++ fields: `max_pack_bytes`, `max_objects_per_pack`, `compact`; exact minimum 17 MiB+8 bytes.

Planning uses stored lengths and checks actual envelopes again. Insufficient catalog slots fail before building: raise limits or compact. Result final-catalog fields `objects`, `pack_count`, `pack_bytes`, `index_bytes` differ from `new_packs`, `objects_written`, `pack_bytes_written`, `index_bytes_written`. Bytes include whole files. `loose_objects_removed` and `obsolete_pack_files_removed` count physical removals. Existing data is still fully audited; incremental means fewer rewrites, not constant-time validation.

## Publication and recovery {#publication-et-reprise}

Under the exclusive Store lock: reload config/indexes and verify all objects/dependencies/file hashes including unpinned data; plan and stream durable packs/indexes; read back every envelope and verify ID/format/authentication; atomically replace `config.json` to publish the entire catalog; then remove redundant loose/inactive generated files.

Failure before config replacement preserves the old catalog. A batch can leave unpublished packs. After publication, interruption can leave duplicates/old packs, but the catalog names complete copies. `pack` or `gc --delete` cleans after validation. Only inactive generated `pack-<32 lowercase hex>.syp/.syi` names are eligible; symlinks are refused.

A flush can fail after atomic replacement when the new catalog is already visible. The handle reloads effective config before returning the error; failed reload invalidates its in-memory index. Fix the IO issue and reopen or complete a fresh audit before object access.

## Selective GC {#gc-selectif}

GC validates every retained closure and complete file hash without caches before deleting. Missing/corrupt retained data stops it. Removing a reference cannot remove blocks shared by another branch/archive/revision/pin.

Each pack: fully live stays unchanged; fully dead leaves the catalog without copying; partly live gets a replacement with only verified survivors. Catalog changes publish together before old files are removed. Live loose objects stay loose. GC does not merge packs; compact separately. A replacement cannot exceed its source count/size, even for an older large pack.

Preview writes no pack/config. `reclaimable_bytes` sums dead envelope copies excluding headers/indexes. `repack_required`, `packs_to_rewrite`, `packs_to_drop`, `pack_bytes_to_write` describe replacement work; the latter includes new pack headers, excludes indexes. `obsolete_pack_files`/`obsolete_pack_bytes` separately describe already inactive leftovers.

## GC memory {#memoire-du-gc}

After interrupted GC, dead loose metadata may temporarily outlive dead packed dependencies. Retained versions remain complete but `fsck` can report orphan dependencies; resume GC before a full audit. See [recovery](doc:crash-recovery#interrupted-garbage-collection).

GC streams 256-entry pages and uses `sum(ceil(pack_objects/8))` mark bytes:128 KiB for 1,048,576 entries in one pack, before overhead. Only survivor count/size per pack are retained; no dead/survivor-ID lists. Streaming output checks count/order and verifies readback before publication with unchanged formats.

Live keys absent from packs still use an ordered set of 33-byte typed keys plus allocation overhead. Loose-only marking is proportional to live objects. Traversal stack, refs, JSON, codecs, directories/filters/caches add memory; no global ceiling. A first loose traversal validates the tree/counters before any replacement/deletion; a second removes dead loose data after publication. Duplicates count once logically and all dead copies contribute reclaimable bytes. Cleanup uses the new catalog, never reinterpreting old bitmap positions.

`packed_mark_bytes`, `loose_live_objects`, `peak_pending_objects` report useful initial bitmap bytes, live keys outside the initial catalog and maximum pending stack keys. They do not measure all allocations/RSS. Root validation ignores caches and both passes/publication stay exclusively locked.

## Current limits {#limites-actuelles}

Catalog limits:1,024 packs,1,048,576 objects/pack,4,194,304 packed objects. Index bytes: `16 * packs + 45 * objects`. Opening validates complete indexes then uses 256-record pages, directory/filter and default 4 MiB LRU (`--index-cache-mib`). Overlapping packs use merged duplicate validation. See [snapshot guarantees](doc:indexes). Limits do not establish multi-million-object load validation.

Writing streams objects without loading whole packs. Compaction needs new-generation disk space before removal and preserves envelopes without recompression/rekeying. Reconstruction/closure/audit sessions share at most eight LRU pack handles, closed on outer return/exception and before publication/removal; isolated reads open independently. Every envelope is reread before cache reuse.

Paged metadata trees, automatic merge-pack selection and concurrent online maintenance remain future work. `objects()` and public closures retain materialized inventories; streaming plans sort only loose keys. `fsck` streams with packed bitmaps. See [compaction](doc:compaction), [audits](doc:audit), [binary format](doc:protocol), [resource budgets](doc:performance) and [limits](doc:limits).
