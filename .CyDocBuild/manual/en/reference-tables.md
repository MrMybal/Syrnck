---
id: reference-tables
title: Reference table parsing and listing
description: Reference table parsing and listing guide.
section: Storage
order: 480
parent: storage
revision: 1
---

# Reference table parsing and listing {#reference-table-parsing-and-listing}

Large Git retention inventories can retain many file manifests independently.
Reference loading and listing now avoid quadratic searches over the complete
table without changing its persisted or HTTP representation.

Local `refs.json` and `Remote::refs` use a tree-backed JSON object before
validating names and object keys into `Refs`. Serialization appends the sorted,
unique `Refs` entries directly to the ordered JSON object. The CLI, HTTP server
and atomic reference writer share that encoder. Output order, field order and
bytes remain unchanged. There is no new storage format, object ID or ABI.

## JSON validation {#json-validation}

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

## Runtime costs and limits {#measured-cli-listing-windows-x64-4-november-2026}

Synthetic measurements do not establish production throughput. Account for object counts, metadata, physical layout, verification and external IO contention.

## Observer changes during batch publication {#observer-changes-during-batch-publication}

Batch CAS retains the original bounded reference document while validating its
candidate closures and reporting progress. After the last observer, it checks
that the persisted document still matches those original bytes. An observer's
reference insertion, removal or replacement therefore makes the batch return
`false` without overwriting that update, including references outside the batch.
A fresh retry uses the resulting table. Rewriting an identical canonical table
is a no-op and can proceed. Cancellation still precedes publication.

The additional check is a linear read/byte comparison of at most 16 MiB; it does
not parse another reference DOM or change the disk format. This fences reference
changes through callbacks, not arbitrary filesystem tampering or general
concurrent access to one Store handle. Normal exclusive Store locking remains
required.
