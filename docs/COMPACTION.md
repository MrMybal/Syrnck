# Streaming packing and compaction

`syrnck pack` appends packs for new loose-only objects. `syrnck pack --compact`
regroups all active objects, including unreachable objects, into new packs. Use
`gc` to remove unreachable objects according to references and pins. Compaction
preserves complete manifests and every block needed to reconstruct any stored
revision; it does not introduce chains of binary deltas.

Both operations audit the store first, including complete file hashes and both
copies of loose/packed duplicates. They preserve each selected envelope exactly,
without recompression or encryption-key changes. Reads prefer a loose copy when
the same object is also packed.

## Ordered input and two passes

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
and crash-recovery rules are unchanged; see [pack recovery](PACKS.md).

## Memory and remaining limits

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

## Tests and reproduction

Native tests cover interleaved input packs, partial/exact page boundaries, zero
index cache, handle limits, early cursor cancellation, changed index pages,
capacity preflight, changed source count/IDs/lengths, second-pack failure, mixed
stores, encrypted envelope preservation and no-op maintenance. The existing
publication, corruption, byte-budget, archive and reconstruction tests also apply.

The compaction benchmark links the same `examples/compact_bench.cpp` harness to
two actual engine revisions. It prepares valid objects with `syrnck_gc_bench`,
copies the fixture before each run, and measures the complete `Store::pack()`
operation: initial audit, planning, writing, readback, publication and cleanup.
An additional full-file audit and reference comparison run after time and memory
are captured. Three repetitions alternate the two binaries in new processes.

```text
python scripts/benchmark_compaction.py --before-exe BEFORE --after-exe build/Release/syrnck_compact_bench.exe --fixture-exe build/Release/syrnck_gc_bench.exe --before-revision REVISION --output compact-report.json
```

To reproduce the baseline, extract the named revision to a separate directory and
append this measurement-only target to its `CMakeLists.txt`, pointing both builds
at the same harness source. Leave the archived engine sources unchanged:

```cmake
add_executable(syrnck_compact_bench "/absolute/path/to/examples/compact_bench.cpp")
target_link_libraries(syrnck_compact_bench PRIVATE syrnck_core)
if(WIN32)
  target_link_libraries(syrnck_compact_bench PRIVATE psapi)
endif()
```

Configure the baseline with `SYRNCK_BUILD_BENCHMARKS=OFF` to avoid a duplicate
target. Network, CLI and tests can also be disabled for this core-only build.
Use the same compiler, Release settings and pinned dependencies for both builds.
On Unix, replace the executable paths with their Unix locations; memory counters
currently return `null` there. OS caches and surrounding load are uncontrolled.
The 64-byte-block fixture measures inventory overhead rather than realistic
Unreal assets or default CDC boundaries. Plaintext timing and memory measurements
do not replace the encrypted-store functional tests.

## Windows x64 measurement — 4 November 2026

The baseline is engine commit `5bca803b534e7c732c34af247a8d77f085e9c5ab`.
Only the measurement target was appended to that archived checkout; its engine
sources were unchanged. Both binaries use the same harness, MSVC 19.40, Release
`/O2 /Ob2`, and pinned dependencies. The baseline was built with CLI/network/tests
disabled, while the current checkout retained its normal build configuration;
the harness links only the core in both cases.

The fixture contains 131,072 distinct 64-byte chunks, 64 valid manifests and one
revision: 131,137 objects in one pack. Half the files are unreachable from the
retained revision. Each run compacts all objects into three packs, with limits of
65,536 objects and 512 MiB per pack. Generation and fixture copying are excluded
from the measurement. Medians over three alternating runs:

| Engine | Process peak working set | Complete compaction |
| --- | ---: | ---: |
| Commit `5bca803` | 50.72 MiB | 20.764 s |
| Streaming plans | 16.30 MiB | 22.079 s |

Peak working set is approximately 3.1 times smaller for this fixture. This run
does not establish a throughput improvement: the new median is 6.3% longer, and
individual timings range from 16.807–21.357 s before and 16.156–22.988 s after.
OS caches and surrounding activity are uncontrolled. The result supports the
memory reduction, with further throughput profiling still needed.

Both paths read and decode 393,411 objects during the measured operation: the
initial audit, rewrite verification and output readback. They produce identical
logical/physical counters, publish three packs totaling 15,297,716 bytes, remove
the old pack/index and preserve every reference. Each separate post-compaction
audit verifies all 131,137 objects and the complete hashes of all 64 files.

The [complete report](benchmarks/streaming-compaction-windows-x64-2026-11-04.json)
records all six runs, binary SHA-256 values, the baseline revision, fixture,
counters and memory snapshots. Process memory includes runtime, opening, initial
audit, temporary allocations, metadata, codecs and caches. It excludes the later
audit and does not measure the OS-wide file cache.
