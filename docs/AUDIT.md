# Streaming integrity audits

`Store::fsck()` and `syrnck fsck` verify every logical object in the active store,
including unreachable revisions, manifests and chunks. This is a read-only audit;
retention and deletion remain the responsibility of `gc`.

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

## Memory and lifetime

Packed objects use one mark bit per index entry, rounded up separately for each
pack. For 1,048,576 entries in one pack, the useful bitmap is 131,072 bytes. The
page directory, negative-key filter and configurable index page cache have their
own costs, described in [paged indexes](INDEXES.md).

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
collections. Packing/compaction now use [streaming plans](COMPACTION.md), while
their loose-only input keys still need sorting.

No storage, network, C ABI or logical object format changes are needed.

## Verification and measurement

The native audit tests exercise loose, packed and mixed stores, multiple packs,
encrypted and plaintext envelopes, unreachable graphs, physical duplicates,
missing dependencies/references, invalid loose entries and failed-audit recovery.
Paged-index tests cover kind boundaries, absent kinds, original positions,
reentrant reads and zero-cache operation. Existing reconstruction, cache and GC
tests continue to apply.

The reproducible benchmark uses valid compressed objects and checks complete
hashes of both retained and unreachable files:

```text
python scripts/benchmark_audit.py --exe build/Release/syrnck_gc_bench.exe --output audit-report.json
```

On Unix, use `build/syrnck_gc_bench`; process memory counters currently return
`null` there. Fixture preparation occurs in a separate process. Every run starts
a new process, disables decoded caches, keeps the default 4 MiB index page cache
and checks that the store's files remain byte-identical.

The comparator reproduces the previous `fsck` collections using the current
codecs and indexes in the same binary. It is not a historical executable. Its
auxiliary catalog reader uses no page cache, and traversal differs from streaming
enumeration; timing is not an exact before/after product comparison. The small
64-byte blocks isolate inventory costs rather than represent Unreal assets or
the default CDC configuration. OS caches and surrounding load are uncontrolled.

On Windows x64 Release (MSVC 19.40), three alternating repetitions over 131,072
distinct chunks, 64 manifests and one revision produced these medians:

| Path | Process peak working set | Audit time |
| --- | ---: | ---: |
| Materialized comparison | 85.27 MiB | 8.537 s |
| Streaming audit | 11.64 MiB | 8.312 s |

The peak working set was approximately 7.3 times smaller for this fixture. The
bitmap's useful size is 16,393 bytes; it is only part of the process memory.
Both paths read and decoded exactly 131,137 objects per run, verified all 64 file
hashes and left the store unchanged. Half of these files were unreachable from
the retained revision. Small timing differences do not establish a general
throughput gain.

The [complete report](benchmarks/streaming-audit-windows-x64-2026-11-04.json)
records every run, memory counters, object counters, fixture descriptor and the
SHA-256 of the measured binary. The Windows peak includes process/runtime,
opening, temporary allocations, metadata and caches; it is not just the mark
bitmap or the OS-wide file cache. See the existing [memory counter definitions](PERFORMANCE.md#index-paginés-et-mémoire--4-novembre-2026).
