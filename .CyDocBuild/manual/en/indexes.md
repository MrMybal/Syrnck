---
id: indexes
title: Paged pack indexes
description: Paged pack indexes guide.
section: Storage
order: 10
parent: storage
revision: 1
---

# Paged pack indexes {#lecture-paginee-des-index-de-packs}

The engine reads `SYRIDX01` indexes directly from disk, retaining page fences/digests, a negative-key filter and a bounded page cache instead of a complete ID/location table. No physical v2/v3, object, envelope or network bytes change; no auxiliary index or automatic conversion is added.

## Validation and lookup {#validation-et-recherche}

Opening and auditing stream the entire index with 256 records (11,520 bytes). Header/count/types, strict order, contiguous ranges and the configuration’s complete BLAKE3 index hash are verified. Each page records first/last key and digest; each pack has a roughly 10-bit-per-entry presence filter with seven probes.

A negative rejects absent keys without page IO. A positive still requires the index; it does not validate an object. Binary search selects a candidate page. Index file identity and size are checked when opened; each reread page must match its validated digest before addresses are used. Changed, truncated or missing pages fail.

Indexes are independently sorted. Overlapping key intervals trigger a merged traversal checking that no key belongs to two active packs. This holds one page per participating pack plus a head heap: up to 11.25 MiB of page data at 1,024 packs. Disjoint intervals avoid this merge.

## Cache and snapshot {#cache-et-snapshot}

Verified hot pages and the last address are a snapshot of the opened catalog under its lock. External index changes may remain invisible on hot entries; reread pages must match validated digests. `fsck` and `gc` reload and completely hash indexes rather than using old pages as evidence.

This concerns addresses only. Envelopes are still reread and checked before decoded-cache reuse; authentication, ID and format rules are unchanged. Filters, directories and pages retain no objects.

## Configuration and counters {#reglages-et-compteurs}

The page cache defaults to 4 MiB, independently of the 64 MiB decoded cache. The global option accepts 0..1024 MiB; zero disables page/last-address caching while retaining directory/filter.

C++ uses `StoreOptions::pack_index_cache_bytes` as the fourth `Store` constructor argument. Maintenance’s temporary readers inherit it. It budgets page data per reader, shared across that catalog’s indexes; temporary snapshots, structures and buffers add costs. Oversized pages use temporary buffers without admission.

`io_stats()` reports `index_directory_bytes`, `index_filter_bytes`, `index_cached_bytes`, `index_cached_pages`, including useful capacities but excluding allocator/container overhead, paths, JSON, temporary buffers and OS pages. They are not process RSS. `clear_cache()` releases decoded/page caches and keeps the validated directory; `reset_io_stats()` keeps caches and memory snapshots.

Indexes contain no decoded file data; types, IDs, sizes and offsets remain visible. Reconstruction, closure, ingest and inventory sessions reuse at most eight LRU index handles plus eight possible data-pack handles. Nested sessions share them; outer return/exception and retirement close them. Isolated lookups open/close their missing-page handle.

```text
syrnck --index-cache-mib 4 get <manifeste> nouvelle-copie.bin
syrnck --cache-mib 0 --index-cache-mib 0 fsck
```


## Memory and limits {#memoire-et-limites}

The directory uses 98 bytes per page and the filter approximately 10 bits per entry. At 1,048,576 entries: 401,408 directory bytes, 1,310,720 filter bytes, before cache/overhead; the unchanged index file is 47,185,936 bytes. Index construction also streams records instead of preparing the entire file buffer.

Limits remain 1,048,576 objects/pack, 1,024 packs, 4,194,304 packed objects total. Opening validates linearly; random lookups may cost more IO than an in-memory table, especially without cache. Filters reduce but do not eliminate false positives.

GC marks packed entries with one bit and streams dead/survivor inventories and replacement readback. Enumeration pages own their buffers, safe across nested lookups even without cache. `fsck` likewise enumerates revisions, manifests and chunks by type/page fences without a full inventory, preserving boundary positions. See [audits](doc:audit).

Public `objects()`/`closure()` still materialize IDs. Packing/compaction retain only loose keys for sorting; see [streaming plans](doc:compaction). GC/audit still need compact IDs absent from packs, dependency stacks and current-file JSON. There is no global project-independent RAM ceiling. See [GC budgets](doc:packs#memoire-du-gc) and [performance](doc:performance).
