# Manifest comparison and chunk reuse plans

`Store::compare_files(source_manifest, target_manifest, operation)` compares
two verified v1 manifests without reading their file payloads. Its ordered
English JSON has version 1, the source/target IDs and sizes, `same_content`,
`reused_bytes`, `new_bytes`, `unique_new_bytes`, `blocks` and `needed_chunks`.

Each target block has its `id`, target `offset`, `size` and `source_offset`.
The latter is the first matching chunk's byte offset in the source, or null
when the source manifest has no matching chunk. Moved and repeated chunks are
reusable. Their declared sizes must agree everywhere an ID appears.
`needed_chunks` lists each absent chunk once, in first target occurrence order.

`reused_bytes + new_bytes` equals the target file size. `new_bytes` counts every
target occurrence; `unique_new_bytes` counts distinct absent chunks once. These
are logical plaintext bytes. They exclude manifests, protocol framing, encryption
and compression, and do not measure network traffic. An already populated
destination Store may need fewer objects still.

`same_content` compares the declared size and complete file hash independently
of chunk layout. Identical bytes can have different chunk boundaries and yield
no chunk reuse. This planner does not search inside chunks or emit binary deltas.
It builds a reconstruction recipe without changing stored formats or references.

This is a metadata plan: source and target chunks may be absent. A source byte
offset is not proof that a mutable local file still has those bytes. Consumers
must verify copied/imported chunks against their IDs and verify the completed
file against its target manifest before publication. Existing verified object
import, reconstruction and reference publication provide those checks.

Only one manifest is read when both IDs are equal, otherwise two. Metadata and
the emitted plan grow with their block counts, under the existing 16 MiB limit
per input manifest. Lookup costs are O((source + target blocks) log(unique IDs));
no file-size payload buffer is allocated. Manifest parsing still reads complete
metadata and has no persistent index.

Progress uses `planning`; its counters represent processed block entries across
both manifests, rather than file bytes. Cancellation and callbacks follow the
[operation contract](OPERATIONS.md). The operation is read only and does not
pin its input manifests; callers must maintain their intended retention roots.

Tests reconstruct independently from source bytes plus needed target chunks,
check repeated/moved blocks, exact accounting, empty files, changed chunking,
inconsistent metadata, cancellation and plans with unavailable source payloads.
