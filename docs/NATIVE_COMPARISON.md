# Native working-directory status and snapshot comparison

The native engine can inspect a working directory and compare two revision
snapshots without importing files or publishing references. These operations
return canonical English JSON. Progress and diagnostics support English and
French, using the ordinary `--lang`, `--progress` and `--progress-json` options.

```text
syrnck native status
syrnck native status --branch feature --directory PROJECT
syrnck native status --revision REVISION --progress-json
syrnck native diff SOURCE_REVISION TARGET_REVISION
```

## Working-directory status

The default baseline is `heads/main`. `--branch NAME` selects another native
branch; a missing branch is an empty baseline, with every selected file reported
as untracked. `--revision ID` selects an exact revision and cannot be combined
with `--branch`. A missing or invalid explicit revision is an error.

`status` reads every selected regular file in full and computes its BLAKE3 file
hash. A matching size or modification time never substitutes for reading the
contents. This detects edits that preserve both size and timestamp. The report
contains `base`, `clean`, `tracked_files`, `workspace_files`, `hashed_bytes`,
`counts` and sorted `changes`.

A clean result compares working bytes with manifest hashes; it does not audit
the stored chunk payloads.

| Status | Meaning |
| --- | --- |
| `untracked` | A selected working file has no baseline entry |
| `removed` | A baseline file is absent from the selected working snapshot |
| `modified` | A common path has different content or executable metadata |
| `unchanged` count | Content and executable metadata match the baseline |

Each change has `path`, `status`, `source`, `target`, `content_changed` and
`executable_changed`. A missing side is null. File summaries contain `manifest`,
`size`, `file_hash` and `executable`; a working file has a null manifest because
status does not ingest it. `content_changed` is true for additions/removals;
`executable_changed` compares modes only when both sides exist. `clean` requires
no changes, including untracked files. A successful dirty status still exits 0.

The scanner is shared with `native commit`: nested `.syrnckignore` rules,
negations, `.git`/`.syrnck` exclusions, the Store directory, portable path checks
and case-folding collision rules apply to both. The Store must reside inside
the inspected directory. An ignore file must be a regular file. Selected
symlinks and special files are rejected; excluded paths are not followed.

Ignore rules apply to the complete candidate snapshot, including tracked files.
Adding a tracked file to `.syrnckignore` therefore reports it as `removed`, even
if it still exists on disk. The next ordinary native commit also excludes it.
This follows the current native commit contract. There is no staging index yet.
Executable bits are inspected on Unix; native Windows scans record false, as
native commits already do.

## Revision snapshot comparison

`native diff` compares the two snapshots' file entries, using only verified
revision and manifest metadata. It requires all file manifests, including
unchanged files, but reads no chunk payloads and does not traverse revision
parents. It can inspect metadata-only partial Stores. It does not prove that
the referenced chunks or historical parents are available; use `fsck` for a
complete integrity audit.

The report contains `source`, `target`, `equal`, `source_files`, `target_files`,
`counts` and sorted `changes`. Status names are `added`, `removed` and `modified`,
with an `unchanged` count. Change summaries have the same fields as working
status, plus `manifest_changed`. A different manifest can describe identical
file bytes with different chunking: that entry is modified with
`manifest_changed: true` and `content_changed: false`. Executable-only changes
are also distinguished. Revision messages, timestamps and parents do not affect
file-entry equality. Renames are reported as removal plus addition.

This command compares snapshot metadata. Text patches, rename inference,
staging, checkout into an existing directory and merge/conflict resolution
remain future work.

## C++ and operation boundaries

```cpp
auto status = store.status_directory(project, baseline_revision, operation);
auto empty_baseline = store.status_directory(project);
auto difference = store.compare_revisions(source_id, target_id, operation);
```

`baseline_revision` is an optional string; omitting it selects an empty baseline
in the library. Branch resolution belongs to the CLI. The C ABI and Go binding
do not yet expose these native revision operations.

Callbacks are synchronous on the calling thread and must not reenter the Store.
`scanning` identifies visited directories, `verifying` reports per-file bytes,
and `planning` counts metadata/comparison entries. Counters reset between stages.
Cancellation or observer failure aborts without returning a partial report or
changing objects, references or working files. CLI JSON is written only after
success. The usual exclusive Store lock and normal opening/recovery rules apply.

The content buffer is 1 MiB. A per-operation FIFO caches at most 1,024 manifest
summaries. Snapshot tables and the complete report remain in memory, and each
manifest/revision retains its 16 MiB logical limit. The shared scanner rejects
a provably oversized flat snapshot before its file-entry lower bound exceeds
16 MiB. This is not a global process-memory limit or a persistent work index.

Status detects size, timestamp, type and Unix-mode changes observed around an
individual read, including growth during hashing. It does not lock the working
directory or provide an atomic filesystem snapshot. Concurrent changes can
require retrying; changes after a file was read can make a report stale. A
watcher, persistent index and coordinated workspace transactions remain future
work.

Regression tests cover empty snapshots/files, equal-size/equal-time edits,
nested ignores, tracked exclusions, file/directory transitions, Unicode paths,
read-only Store snapshots, metadata-only partial Stores, modes, rechunking,
cancellation, callback failures and source mutation. Plain/encrypted and
loose/single-pack/multi-pack variants run locally on Windows x64. Unix-specific
paths await execution in the existing CI matrix.
