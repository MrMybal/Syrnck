# Progress and cooperative cancellation

The C++ ingestion, native commit, reconstruction and Git-filter interfaces can
report progress and accept cancellation. Native commits and clean filters use
repository pipeline defaults; explicit `IngestOptions` replace those defaults.
The CLI accepts `--workers` and `--buffer-mib` for `put`, `native commit` and
`git clean` without changing saved preferences.

## CLI

```text
syrnck config set --workers 4 --buffer-mib 32
syrnck put Content/Map.uasset --progress
syrnck native commit -m "Snapshot" --progress-json
syrnck get MANIFEST restored.uasset --progress
```

`put`, `get`, `fsck`, `native commit`, `native export`, `git clean` and `git smudge` accept mutually
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

These commands handle Ctrl+C, Windows Ctrl+Break and SIGINT/SIGTERM where
available. Cooperative cancellation reports an English/French diagnostic and
exits with code **130**. The handler only records an atomic flag; an ordinary
monitor thread requests the C++ stop token. A read blocked on a pipe or device
must return before cancellation can finish. Repeated interrupts request the
same stop; forced process termination still uses the separate crash-recovery
path. Network, packing and GC do not yet use this controller.

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

`IngestOptions::progress` receives a `ProgressEvent` synchronously on the calling
coordinator thread. Phases are scanning, ingesting, reconstructing and publishing.
Events contain processed bytes, an optional total, optional path context, a
per-file completion flag and the current buffered-block count. Native commits
provide each relative file path and its observed size. An ordinary stream import
has no known total. Empty files still report progress and completion.

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
publication, that complete unreferenced manifest can also remain. A completion
event describes the current file's processing, rather than final branch/pin or
destination publication.

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

These options do not yet extend to GC, packing or network transfers.
Process-kill recovery remains a separate mechanism described in
[crash recovery](CRASH_RECOVERY.md). No object, pointer, network or C ABI format
changes are needed; C++ callers should rebuild against the updated headers.
