# Reference table parsing and listing

Large Git retention inventories can retain many file manifests independently.
Reference loading and listing now avoid quadratic searches over the complete
table without changing its persisted or HTTP representation.

Local `refs.json` and `Remote::refs` use a tree-backed JSON object before
validating names and object keys into `Refs`. Serialization appends the sorted,
unique `Refs` entries directly to the ordered JSON object. The CLI, HTTP server
and atomic reference writer share that encoder. Output order, field order and
bytes remain unchanged. There is no new storage format, object ID or ABI.

## JSON validation

The depth guard now runs as a SAX pass before ordinary DOM parsing. The previous
DOM callback parser scanned each parent container for discarded values after
every completed child, even though Syrnck's callback never discarded anything.
This made wide arrays of objects quadratic as well as large reference tables.
The SAX guard avoids that parent scan and does not construct a DOM of its own.
Immutable metadata still uses ordered JSON and the same canonical validation.

The existing 128-level depth rule is preserved, including its boundary behavior
for empty containers. Syntax, UTF-8, numbers, names, object kinds, hashes and key
fields remain validated. Reference tables accept arbitrary field order and
retain the previous last-value behavior for duplicate JSON names. A malformed
document is rejected without overwriting the persisted table. The 16 MiB
reference-document limit now applies to reads as well as writes.

Reference parsing performs O(N log N) table insertions; sorted table encoding
performs O(N) entry insertions, excluding key lengths and output bytes. This
does not make all ordered JSON objects linear. Two lexical passes add work for
small documents, and the reference DOM and `Refs` coexist during conversion.
The engine still reads the complete reference document on each access. A
persistent index, incremental reference journal and a global memory budget
remain future work.

## Measured CLI listing — Windows x64, 4 November 2026

Three alternating runs in fresh Release MSVC processes measured the complete
`syrnck refs` command: process startup, Store opening and validation, another
reference read, JSON serialization and stdout capture.

| References | Previous median | Updated median |
| ---: | ---: | ---: |
| 8,192 | 0.599 s | 0.064 s |
| 32,768 | 8.941 s | 0.296 s |

The baseline executable uses `c3e8d94` with the HTTP authentication fix; its
reference paths are unchanged. Executable hashes and all runs are in the
[measurement report](benchmarks/reference-tables-windows-x64-2026-11-04.json).
The fixture contains one valid file manifest and its block, retained by all
references. Fixture generation, Python JSON validation and the final full-file
audit are outside the timings. Both executables produce identical stdout and
leave the Store byte-identical. OS caches and external system load are not
controlled. These are reference-listing measurements, not Unreal asset or
network-throughput benchmarks; no memory improvement is claimed.

Reproduce with two retained CLI binaries:

```text
python scripts/benchmark_refs.py --before-exe PATH_TO_BASELINE --after-exe PATH_TO_UPDATED --before-revision BASELINE_REVISION --output .scratch/reference-tables.json
```

`reference_tables` exercises 32,768 pins in plain and encrypted stores, atomic
updates, reopening, dependency audits, wide JSON arrays, depth boundaries and
parser compatibility. `server_authentication` additionally tests the production
HTTP listing and native agent pull with 16,384 references.
