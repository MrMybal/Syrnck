# Progress and cooperative cancellation

The C++ ingestion, native commit, reconstruction and Git-filter interfaces can
report progress and accept cancellation. Native commits and clean filters use
repository pipeline defaults; explicit `IngestOptions` replace those defaults.
The CLI accepts `--workers` and `--buffer-mib` for `put`, `native commit`,
`native add`, `git clean` and `git migrate-lfs` without changing saved preferences.

## CLI

```text
syrnck config set --workers 4 --buffer-mib 32
syrnck put Content/Map.uasset --progress
syrnck native commit -m "Snapshot" --progress-json
syrnck get MANIFEST restored.uasset --progress
```

`put`, `get`, `compare`, `fsck`, `pack`, `gc`, `native commit/add/staged/reset/status/checkout-plan/checkout/checkout-recover/switch-plan/switch/diff/export`, `git clean/smudge/filter-process`,
`native tag/tags/tag-show/tag-delete/tag-archive/tag-restore`,
`native ancestry/cherry-pick-plan/revert-plan/cherry-pick/revert/merge-plan/merge`,
`git lfs-preview/migrate-lfs/retention-scan/retention` and
`agent push/pull` accept mutually
exclusive `--progress` and `--progress-json`. Both write only to stderr; stdout
keeps its ordinary JSON, revision ID, Git pointer or binary stream. With neither
flag, successful commands stay quiet on stderr. Other commands reject these
flags. Plain source data passing through smudge and existing clean pointers do
not need block processing and may produce no progress events.

Text uses the selected interface language and quotes paths to escape newlines
and control characters. JSON is language-independent, with one object per line:

```json
{"phase":"ingesting","path":"Content/Map.uasset","bytes":1048576,"total_bytes":8388608,"complete":false,"buffered_blocks":2}
```

`total_bytes` is null when unknown. Native commit paths are relative; put/get
report the source/destination argument (Windows paths use forward slashes).
Events are throttled to about ten per second for an unchanged phase/path;
phase/path changes and file completion are always emitted. Scanning and branch
publication events are not aggregate project-byte totals. A nonzero exit code
is authoritative even if an earlier file event had `complete: true`.

LFS preview reports verification progress; migration adds ingestion and branch
publication events. Its prepared Git transaction finishes without cancellation
after the final commit command begins. See [LFS migration recovery](GIT_LFS_MIGRATION.md#cancellation-and-recovery).

[File range reads](FILE_RANGES.md) report bytes relative to the
requested range. [Manifest comparisons](FILE_COMPARISON.md) use planning counters
in block entries across both manifests, rather than bytes. Both support
cancellation before output publication.

[Git retention](GIT_RETENTION.md) scans metadata, verifies observed
assets and publishes all added protections together. Cancellation before that
publication changes no pin; unobserved pins are never removed automatically.

[Native status and snapshot comparison](NATIVE_COMPARISON.md) are read-only:
status hashes selected working files, while revision comparison reads only
revision/manifest metadata. Planning events count entries rather than bytes;
status verification events report bytes within the current file. Cancellation
and callback failure produce no partial report or Store publication.

[Combined status and checkout preflight](NATIVE_WORKSPACE.md) are also read-only.
Combined status keeps the staged and unstaged comparisons separate. Checkout
planning inventories ignored/untracked paths, hashes tracked files and optionally
verifies the complete target closure with `--verify`. It ends with a complete
planning event; blockers are a completed JSON result with CLI exit 1. Neither
operation publishes files/references or emits a publishing callback.

[Native checkout](NATIVE_CHECKOUT.md) journals preparation and sequential file
moves, with explicit resume/abort. Cancellation after journal creation can leave
a partial workspace while the original branch remains unchanged. The last
observer before atomic branch/journal publication is `publishing` with path
`checkout`; afterwards cleanup has no callbacks or cancellation. A cleanup error
can leave a committed journal: inspect `native checkout-status` and resume,
as abort is refused after publication. GC and native workspace writes remain
blocked until the active journal has been cleaned. Prepared targets and original
backups contain plaintext even when Store objects are encrypted.

[Native branch switching](NATIVE_BRANCHES.md) uses the same executor and commit
point. HEAD remains on the source branch until it publishes atomically with
the committed journal; the two branch heads are checked and kept. Abort before
publication retains the old selection. `native head` is read-only and accepts
no progress options; `switch-plan` and `switch` support progress/cancellation.

[Native tags](NATIVE_TAGS.md) support progress/cancellation for creation, paged
listing, inspection, archived deletion and restoration. Mutations verify the
complete retained closure before the `publishing` event at `refs`, then publish
all tag/annotation/archive roots together. Cancellation before that point keeps
the old references; new unreachable metadata may remain. No observer or stop
check follows publication. Read-only reports default to metadata inspection;
`--verify` adds complete target verification.

[Native staging](NATIVE_STAGING.md) freezes selected files during `native add`;
`staged` reads metadata and `reset` changes only the index. Staged commit never
reads working files. Before publication, cancellation or observer failure keeps
both branch and index unchanged; preparation may leave unreachable objects.
The final `publishing` event identifies `refs`. Branch movement and index removal
publish together, with no cancellation or observer call after that boundary.

[Native merge](NATIVE_MERGE.md) and [replay](NATIVE_REPLAY.md) plan without writing
objects or files; `--verify` audits their input closures and any custom resolution
manifests. Apply recomputes a verified plan and uses the recoverable checkout
executor. Cancellation before journal publication preserves branch/workspace;
afterwards inspect/resume/abort the journal as for checkout. Explicit
[resolution documents](NATIVE_RESOLUTIONS.md) bind the inputs and selected parent;
partial choices never publish a partial snapshot.

These commands handle Ctrl+C, Windows Ctrl+Break and SIGINT/SIGTERM where
available. Cooperative cancellation reports an English/French diagnostic and
exits with code **130**. The handler only records an atomic flag; an ordinary
monitor thread requests the C++ stop token. A read blocked on a pipe or device
must return before cancellation can finish. Repeated interrupts request the
same stop; forced process termination still uses the separate crash-recovery
path. Committed maintenance cleanup finishes without further cancellation.

The implementation follows the C++ requirements for
[signal-safe operations](https://eel.is/c++draft/support.signal) and
[lock-free atomic flags](https://eel.is/c++draft/atomics.flag), and the Windows
[console handler contract](https://learn.microsoft.com/en-us/windows/console/handlerroutine).
`cli_control` tests handler restoration and callback framing; `cli_operations`
checks stdout preservation and real OS interruption. Its Windows-only relay
creates a hidden private console and sends a real Ctrl+Break to the CLI; it is
not installed or linked into production code. Local validation was on Windows;
the Unix SIGINT/SIGTERM cases are included for the next platform CI run.

## C++ operations

C++ observers receive a `ProgressEvent` synchronously on the calling coordinator
thread. Phases are scanning, ingesting, reconstructing, verifying, exporting,
transferring, planning, packing and publishing. Events contain processed bytes, an optional total,
optional path context, a completion flag for the reported scope and the current
buffered-block count. Native commits provide each relative file path and its
observed size. An ordinary stream import has no known total. Empty files still
report progress and completion.

Paths are borrowed for the duration of the callback. Copy them if retaining an
event. Callbacks must not reenter or mutate the same Store. An exception stops
the operation and joins ingest workers. Input-byte progress includes queued
work; it does not claim all blocks have already reached durable storage.

```cpp
std::stop_source cancellation;
auto options = store.settings().ingest_options();
options.stop = cancellation.get_token();
options.progress = [&](const syrnck::ProgressEvent &event) {
  // Observe progress here; another thread may call cancellation.request_stop().
};
auto revision = store.commit_directory(project, "main", "Snapshot", options);
```

Cancellation is checked between reads, blocks, directory entries and publications.
Current native IO/codec work may finish before control returns. Worker threads
only encode data; the coordinator performs writes and callbacks. A stopped import
can leave reusable unreferenced blocks. If cancellation arrives after manifest
publication, that complete unreferenced manifest can also remain. A completed
ingestion event describes the current file's processing, rather than final
branch/pin or destination publication.

Native commits validate their complete closure before publishing a branch and
check cancellation through this traversal and the final atomic reference write.
`set_ref`, `compare_set_ref` and `closure` also accept a stop token directly.
Cancellation before publication leaves the previous reference intact. A request
that races with an atomic publication already in progress can arrive too late;
once publication starts, the operation completes that commit point.

`reconstruct` and `restore_file` accept `OperationOptions` with a stop token and
callback. Direct reconstruction into a caller-owned stream can leave partial
output on failure. `restore_file` stages output atomically, checks cancellation
before publication and removes its temporary file on cooperative cancellation.
An existing destination is never overwritten. Git's required filters report
failure rather than presenting a cancelled conversion as successful; clean emits
its pointer only after the manifest has been retained.

`fsck(OperationOptions)` reports aggregate `verifying` progress without building
a complete object list. Bytes count decoded payload reads, including repeated
blocks needed for complete file hashes and secondary packed copies; the total
is unknown. Completion is emitted only after all checks pass. Stop checks run
between objects, blocks and loose-tree entries. Format/index validation already
in progress must finish first. Cancellation or an observer exception closes read
sessions and restores normal cache use without modifying objects or references.
CLI audit failures emit no partial success message on stdout.

`closure(roots, verify_files, stop, progress)` offers the same verification
progress while retaining its existing materialized return value. Callers that
only need to audit the entire store should use streaming `fsck`.

Multi-file native exports stage a complete tree before exclusive publication.
See [export guarantees, progress and recovery](EXPORT.md).

Network requests also support these options, with a separate final remote
publication commit point and exit 3 for uncertain publication. See
[transfer cancellation, progress and recovery](TRANSFERS.md).
Persistent transfer sessions retain their complete source or available downloaded
objects during GC until completed or deliberately discarded; records are checked
before GC planning. `agent prune` previews completed receipts and `agent prune --delete`
removes only those receipts. See [selective retention and pruning](SELECTIVE_RETENTION.md) and
[journal recovery and retention](TRANSFER_SESSIONS.md).
Legacy server upload leases block offline GC until expiry, publication or deliberate
release. [Inventory-enabled uploads](UPLOAD_INVENTORIES.md) retain declared objects
selectively, with committed page/hash validation. See [server leases and fenced publication](SERVER_UPLOAD_LEASES.md).
Active [read leases](SERVER_READ_LEASES.md) retain their complete root closure
during offline GC, even after the source reference disappears. Pull releases them once its verified objects are
local; `leases ... --read` manages their records after server shutdown.

GC and packing allow cancellation during verification, planning, construction
and readback. After their publication/deletion commit point, cleanup finishes
without further callbacks or cancellation. See [maintenance guarantees and limits](MAINTENANCE.md).
Process-kill recovery remains a separate mechanism described in
[crash recovery](CRASH_RECOVERY.md). Object, pointer and C ABI formats remain
unchanged; upload and read leases negotiate HTTP extensions. C++ callers should rebuild
against the updated headers.
