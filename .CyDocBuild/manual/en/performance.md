---
id: performance
title: Performance and resource budgets
description: Performance and resource budgets
section: Storage
order: 90
parent: storage
revision: 1
---

## Choose budgets for the workload {#budgets}

Decoded cache defaults to 64 MiB, verified stamps to 8192 entries and index pages to 4 MiB. These are independent budgets, not a total process-memory bound. Ingestion defaults to one worker with a 32 MiB raw in-flight budget; optional workers add codec/output memory. Transfer request/compression buffers have separate bounds. Tune using [settings](doc:settings), [indexes](doc:indexes), [parallel transfer](doc:parallel-transfers) and [network compression](doc:network-compression).

Objects and flat manifests/revisions are bounded to 16 MiB. Catalog limits are 1024 packs, 1,048,576 entries per pack and 4,194,304 packed entries. Limits do not prove tested project scale. Real Unreal assets may deduplicate poorly when rewritten/compressed/encrypted before ingestion.

## Interpret counters carefully {#measurement}

`io_stats` counts successful envelope reads/bytes, full decodes and cache hits; index memory fields cover useful directory/filter/page capacities, excluding allocator overhead and OS caches. GC reports packed mark bytes, loose live keys and peak pending traversal keys. None is process RSS.

Opening validates full indexes linearly. Streaming audits/GC/compaction reduce packed-ID materialization but loose keys, metadata, stacks, codecs and caches still cost memory. Checkout reconstructs complete changed files, with private disk space and complete validation. Watch can target subtrees while periodically performing full audits. Synthetic benchmark reports and reproduction scripts remain developer records outside this user manual; their timings are not universal throughput promises.
