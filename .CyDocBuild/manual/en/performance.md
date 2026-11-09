---
id: performance
title: Performance and resource budgets
description: Performance and resource budgets
section: Storage
order: 90
parent: storage
revision: 2
---

## Choose budgets for the workload {#budgets}

Decoded cache defaults to 64 MiB, verified stamps to 8192 entries and index pages to 4 MiB. These are independent budgets, not a total process-memory bound. Ingestion defaults to one worker with a 32 MiB raw in-flight budget; optional workers add codec/output memory. Transfer request/compression buffers have separate bounds. Tune using [settings](doc:settings), [indexes](doc:indexes), [parallel transfer](doc:parallel-transfers) and [network compression](doc:network-compression).

Objects and flat manifests/revisions are bounded to 16 MiB. Catalog limits are 1024 packs, 1,048,576 entries per pack and 4,194,304 packed entries. Limits do not prove tested project scale. Real Unreal assets may deduplicate poorly when rewritten/compressed/encrypted before ingestion.

## Interpret counters carefully {#measurement}

`io_stats` counts successful envelope reads/bytes, full decodes and cache hits; index memory fields cover useful directory/filter/page capacities, excluding allocator overhead and OS caches. GC reports packed mark bytes, loose live keys and peak pending traversal keys. None is process RSS.

Opening validates full indexes linearly. Streaming audits/GC/compaction reduce packed-ID materialization but loose keys, metadata, stacks, codecs and caches still cost memory. Checkout reconstructs complete changed files, with private disk space and complete validation. Watch can target subtrees while periodically performing full audits. Synthetic benchmark reports and reproduction scripts remain developer records outside this user manual; their timings are not universal throughput promises.

## Incremental working-file capture {#incremental-capture}

Resident native agents and C/Go workspace captures reuse manifests ingested by
their session when the published parent, context and materialization match and
strong local filesystem identity/change stamps are unchanged. They enumerate the
complete selected tree, including renames, deletions and ignore rules. Closing
and reopening the Store retains session proof; status indexes and notifications
never establish it. Complete object closure verification still runs, including
historical parents and reused manifests; corrupt/missing data refuses publication.

Recent files (two-second guard), active Windows writers and unsupported/remote
filesystems use ordinary ingestion. Stamp checks support local NTFS/ReFS,
Linux ext4/XFS/Btrfs/tmpfs and macOS APFS. Restart, explicit invalidation, failed
native calls or changed parent/selection discard reuse. One-shot `native commit`
continues to ingest all sources.

C capture JSON adds `capture: {version, ingested_files, reused_files,
source_bytes_read, source_bytes_reused}`. Go exposes `CaptureReportContext`;
`CaptureContext` keeps returning its revision ID. C++ exposes `WorkspaceCapture`
in `<syrnck/workspace.hpp>`. Counters cover source reads, excluding metadata,
closure verification and OS caches.

Build `syrnck_capture_benchmark`, then run:

```text
python scripts/benchmark_capture.py --exe PATH_TO_BENCHMARK --mib 128 --files 64 --iterations 5 --output capture.json
```

Add `--encrypted` for at-rest encryption. This synthetic benchmark alternates
full/session captures, includes Store opening and complete publication, records
Store I/O separately, and verifies every result by a full content scan. It does
not establish Unreal-project throughput. Historical verification and flat
metadata remain scaling limits.

Successful workspace captures seed the observer from their complete selected
inventory, so the next native event tick can target its changed paths. Capture
consumes only pre-capture batches; notifications arriving during or after it
remain queued. A periodic audit still forces a full scan. This is an advisory
observation at capture time, not an atomic snapshot of concurrent writers.
