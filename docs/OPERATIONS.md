# Progress and cooperative cancellation

The C++ ingestion, native commit, reconstruction and Git-filter interfaces can
report progress and accept cancellation. Native commits and clean filters use
repository pipeline defaults; explicit `IngestOptions` replace those defaults.
The CLI accepts `--workers` and `--buffer-mib` for `put`, `native commit` and
`git clean` without changing saved preferences.

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

These options do not yet extend to GC, packing, network transfers or multi-file
export. Process-kill recovery remains a separate mechanism described in
[crash recovery](CRASH_RECOVERY.md). No object, pointer, network or C ABI format
changes are needed; C++ callers should rebuild against the updated headers.
