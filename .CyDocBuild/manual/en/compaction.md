---
id: compaction
title: Streaming packing and compaction
description: Streaming packing and compaction guide.
section: Storage
order: 70
parent: storage
revision: 1
---

# Streaming packing and compaction {#streaming-packing-and-compaction}

`syrnck pack` appends packs for new loose-only objects. `syrnck pack --compact`
regroups all active objects, including unreachable objects, into new packs. Use
`gc` to remove unreachable objects according to references and pins. Compaction
preserves complete manifests and every block needed to reconstruct any stored
revision; it does not introduce chains of binary deltas.

Both operations audit the store first, including complete file hashes and both
copies of loose/packed duplicates. They preserve each selected envelope exactly,
without recompression or encryption-key changes. Reads prefer a loose copy when
the same object is also packed.

## Ordered input and two passes {#ordered-input-and-two-passes}

Packed IDs are already sorted within each immutable index. Compaction merges
these indexes with a heap containing one head per pack and one owned page of 256
records per pack. Nested object lookups cannot invalidate the cursor's buffers,
even with the index cache disabled. The cursor uses the existing bounded index
handle pool and closes its session on exhaustion, cancellation or failure.

Only loose-only IDs are collected and sorted, as compact typed keys of 33 bytes
each before vector capacity and allocator overhead. Packed duplicates do not add
another key. An incremental pack consumes this sorted loose sequence; compaction
merges it with the packed sequence. It does not build a complete packed-ID list.

The first pass checks strict input ordering, count, envelope lengths, object and
byte budgets, and output-pack capacity. It retains only a count, a byte size and
a BLAKE3 sequence digest for each output pack. Capacity or malformed-source
failures occur before payload reads or output creation.

The second pass requests a fresh cursor and writes the planned packs in order.
Each pack's digest binds the typed IDs and selected envelope lengths to the first
pass. Changed IDs, lengths or counts fail the batch. Source or codec errors also
stop the operation. Previously completed files in a failed batch remain inactive
until a later maintenance cleanup; they do not replace the active catalog.

Every new envelope is read back and verified by page enumeration before a single
atomic catalog publication. All old-catalog cursors and read sessions end before
publication. A fresh loose-tree scan then removes copies whose keys are confirmed
in the published catalog. Inactive generated packs are removed last. Publication
and crash-recovery rules are unchanged; see [pack recovery](doc:packs).

## Memory and remaining limits {#memory-and-remaining-limits}

There are no complete packed-ID lists for planning, output partitions, readback
or loose cleanup. The merged input retains at most 11,520 bytes of record data
per active pack, or 11.25 MiB at the existing 1,024-pack limit. Heap entries,
cursor metadata, catalog snapshots, index directories/filters, page caches,
envelopes, codecs and the initial integrity audit have additional costs.

Loose-only ID sorting remains proportional to the number of loose-only objects.
The initial audit's loose marks and metadata also have proportional costs.
Public `objects()` and `closure()` still return complete collections. This is not
a global constant-memory guarantee for the engine, nor does it raise catalog
limits or provide concurrent online maintenance. The exclusive store lock covers
the operation. No on-disk format, object ID, CLI option or C ABI changes are needed.

## Planning maintenance {#tests-and-reproduction}

Compaction requires space for the complete new generation before old packs are removed. Use a verified backup and inspect the result before destructive collection. See [pack publication and recovery](doc:packs#publication-et-reprise).

## Performance boundaries {#windows-x64-measurement-4-november-2026}

Performance depends on the object layout, codecs, storage and cache state. Streaming plans reduce packed-ID inventories; they do not guarantee higher throughput. See [resource budgets](doc:performance).
