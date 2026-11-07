---
id: storage
title: Storage and reconstruction
description: Storage and reconstruction
section: Storage
order: 100
revision: 1
---

## Objects and complete versions {#objects}

A typed BLAKE3 ID addresses a chunk, manifest or revision. A manifest records the ordered complete block list, total length, file hash and chunking policy. A revision contains a complete path-to-file snapshot and parents. Sharing blocks never creates a dependency on replaying an older file version.

`put` retains the returned manifest under `files/ID`; `get` restores it exclusively to a new path. `inspect`, `compare`, `diff` and `refs` inspect metadata. See [manifest comparison](doc:file-comparison) and [verified ranges](doc:file-ranges). Full reconstruction checks every selected chunk and the complete file hash.

## Retention before collection {#retention}

Branches, tags, staging snapshots, archives and explicit pins retain their dependency closures, including revision parents. Removing a visible commit does not necessarily remove its blocks. First inspect what retains the data; then explicitly retire roots with a [verified backup](doc:archive-retention); only then consider destructive GC. GC previews by default and refuses missing/corrupt retained dependencies before deletion.

[Completed checkout roots](doc:checkout-retention), [transfer journals](doc:selective-retention) and active leases have their own protection rules. Backup and restore are logical snapshots, not copies of uncommitted working files. Keep externally edited files stable while the engine scans them.

## Physical storage and budgets {#physical}

New stores use loose v1 objects. Explicit packing selects physical v2/v3 while retaining logical v1 IDs and envelopes. Read [packs](doc:packs), [paged indexes](doc:indexes), [compaction](doc:compaction), [integrity audits](doc:audit) and [maintenance cancellation](doc:maintenance).

Defaults: FastCDC 256 KiB minimum, 1 MiB target, 4 MiB maximum; optional fixed chunking. Uncompressed objects and flat manifests/revisions are limited to 16 MiB. Decoded cache defaults to 64 MiB, verified stamps to 8192 entries, index-page cache to 4 MiB. Cache budgets do not bound total process memory. [Repository preferences](doc:settings) control ingest workers and pack creation separately.
