# Verified file ranges

The C++ Store can read a byte range from an existing file manifest without
reconstructing the complete file:

```cpp
store.reconstruct_range(manifest, output_stream, offset, length, operation);
store.restore_file_range(manifest, new_destination, offset, length, operation);
```

Offsets and lengths are unsigned 64-bit byte counts. Both are exact: the range
must fit inside the declared file size, with no truncation or integer wraparound.
A zero-length range is valid at any offset through EOF. The destination contains
only the requested bytes, starting at destination offset zero.

The manifest is read and validated, then only intersecting chunks are loaded.
Each selected chunk is verified in full through the existing content-addressed
object reader, including authentication/decompression for encrypted objects.
Its actual length must match the manifest before any of its bytes are emitted.
Unrelated chunks may be absent; their availability and integrity are not audited.

A partial read cannot validate the complete file hash. A range covering the
entire file, including an empty file, uses ordinary reconstruction and verifies
that hash as well. To verify every dependency and the whole file before use,
retain the existing full reconstruction or closure verification operation.

Stream output may contain a prefix if a later chunk, callback or output write
fails. File output is prepared in a temporary sibling and published exclusively
after success. Existing destinations are preserved; cancellation, invalid ranges
and corrupt selected chunks do not publish a destination.

Progress uses `reconstructing`, counting bytes relative to the requested range;
`total_bytes` is its length. Cancellation and callback rules follow
[the operation contract](OPERATIONS.md). The Store's existing exclusive lock
and caller serialization requirements still apply.

This operation works with existing v1 manifests and loose, single-pack or
multi-pack storage. It parses the complete manifest and scans block metadata
to locate the range; it does not introduce a paged manifest or a persistent
offset index. Payload memory is bounded by one selected decoded chunk, alongside
manifest metadata, codec scratch and configured caches. At a chunk boundary,
even a small range can require reading and decoding an entire chunk.

Regression tests measure object reads for single-chunk, cross-boundary and
zero-length requests. They also cover all physical layouts, encrypted stores,
missing unrelated chunks, corrupt selected chunks, forged whole-file hashes,
overflow, exclusive output and cancellation. Local execution is Windows x64;
other platforms remain subject to their CI runs.
