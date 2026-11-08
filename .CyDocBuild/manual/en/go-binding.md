---
id: go-binding
title: Go binding
description: Go binding guide.
section: API
order: 10
parent: integrations
revision: 5
---

# Go binding for the C++ engine {#binding-go-du-moteur-c}

Local module `syrnck.local/bindings/go` exposes Stores, CDC/fixed ingest, verified object IO/import, closures, pins and reconstruction with no external Go dependencies. Consumer projects currently need a local `replace` until a permanent module address is published.

The engine remains C++20. Windows amd64/arm64 uses the system DLL loader without cgo or a C compiler; Linux/macOS uses cgo linking `syrnck_ffi`. Windows amd64 has local execution coverage; Linux/macOS and ARM64 still require validation.

## Load the native library {#tester}

Build the matching C++ library. On Windows set `SYRNCK_LIBRARY` to the absolute trusted DLL path before first `Open`; it stays loaded for process lifetime. Unix uses cgo: configure `CGO_LDFLAGS=-L<directory>` and the loader path or distribution rpath. Go 1.22+ is required.

## Example {#exemple}

`Create` requires a new/empty directory; it is not open-or-create. `Options.Key` has zero or 32 bytes. The Go array is not retained. The application owns key lifetime and clearing its copies. The example copies a complete retained file between two local Stores.

```go
source, err := syrnck.Open("source-store", syrnck.Options{})
if err != nil { return err }
defer source.Close()
destination, err := syrnck.Open("destination-store", syrnck.Options{Create: true})
if err != nil { return err }
defer destination.Close()

id, err := source.Ingest("Map.uasset", syrnck.Chunking{})
if err != nil { return err }
stats, err := syrnck.CopyMissing(ctx, source, destination,
    syrnck.ObjectKey{Kind: "manifest", ID: id}, "sync/map-v1")
if err != nil { return err }
fmt.Printf("%d bytes copied\n", stats.Bytes)
return destination.Restore(id, "Map-restored.uasset")
```


## Prepare and publish compound state {#catalog-transactions}

`PrepareBytes(data, chunking)` ingests at most 16 MiB from memory without a
plaintext temporary file. `PrepareFile(path, chunking)` streams a file without
that input limit. Both return **unpinned** manifests, unlike the legacy `Ingest`
method. `Put(kind, data)` creates an unpinned typed object: chunks stay exact;
C++ validates and canonically encodes manifest/revision JSON and computes its
native BLAKE3 identity. Unknown fields, duplicate/escaped duplicate keys, invalid
schemas, excessive nesting and oversized objects are rejected. Go callers do
not need to reproduce the native canonical encoder or object hash algorithm.

Keep the Store open through preparation and publication. GC after closing can
collect unpinned objects. Retain all data dependencies in a complete native
revision or explicit roots; an opaque chunk containing object IDs does not
automatically retain the referenced objects. A native revision's files and
parents trace their complete dependencies through GC and backups.

`Refs()` returns an independent `map[string]ObjectKey`. `CompareRefs(updates)`
atomically guards and replaces/removes a group of up to 65536 references, with
at most 16 MiB of JSON. Every replacement closure and whole-file hash is verified
before one reference-table publication. Duplicate names are errors. A nil
`Expected` requires absence; a nil `Value` removes the reference. Nil/empty groups
are no-ops. The result is meaningful only when `err == nil`:

```go
current, err := store.RefsContext(ctx)
if err != nil { return err }
var expected *syrnck.ObjectKey
if old, ok := current["ffi/catalog/main"]; ok { expected = &old }

// Application-defined JSON describes a complete native revision whose files
// reference the prepared manifests, with any required prior revision parents.
candidate, err := store.PutContext(ctx, "revision", revisionJSON)
if err != nil { return err }
applied, err := store.CompareRefsContext(ctx, []syrnck.RefUpdate{
    {Name: "ffi/catalog/main", Expected: expected, Value: &candidate},
})
if err != nil { return err } // Reload and establish the outcome before retrying.
if !applied { return errRefreshAndReplan }
```

A stale expectation changes no member and returns false without an error.
Refresh/replan instead of changing the expected value of an old operation.
An IO failure can occur after atomic publication: on error, the boolean is not
evidence that publication did not happen. Reopen/read the table and compare the
exact proposed group before retrying. The table is always wholly old or wholly
new, never a mixture produced by this transaction. Persist intent where a
higher-level workflow must reconcile an unknown outcome after a crash.

All five operations have `Context` variants. Preparation and CAS propagate
native cancellation through ingestion/verification and before publication;
successful completion wins a late stop request. `PutContext` and `RefsContext`
check cancellation before their bounded native call; encoding/compression/read
already in flight can finish. Failed preparation may leave complete unreferenced
objects, with no publication. Concurrent external file writers remain the
application's responsibility. These primitives support durable protocol state;
they do not implement Syncthing folder reconciliation or workspace checkout.

## Concurrency and retention {#concurrence-et-conservation}

Do not copy a `Store`. Methods/Close share a mutex. Native call/error retrieval stays on the same OS thread; no Go pointer is retained by C++. Close explicitly to release the process lock.

`Ingest` pins automatically under `ffi/<manifest>`. `Import` verifies ID/format without publication. `Pin` verifies the complete closure/file hashes before replacement; distinct names retain versions. `Unpin` may make data eligible for GC.

`CopyMissing` is a local two-handle integration: stable lock order, verified presence comparison, one object at a time, then pin publication. Retry reuses objects across reopen. Cancellation is between objects. Interrupted unpublished data needs protection from GC until resume; this binding cannot run GC concurrently, but collection after close can remove unpinned data.

Object bodies are 16 MiB maximum; complete closure lists load in memory up to 128 MiB in Go. Buffer reads currently query size then read, verifying twice. Pagination, persistent leases, buffer reuse and boundary overhead remain performance work.

## Ranges and file comparison {#plages-et-comparaison-de-fichiers}

`ReadRange(id,offset,length)` returns an exact range up to 16 MiB/call. Selected chunks are verified; a full-file range also checks the complete hash. `RestoreRange` streams to a new exclusive destination without that length cap. Out-of-range requests fail without truncation.

`CompareFiles(source,target)` returns typed reusable/new bytes, target blocks with optional source offset and a deduplicated list absent from source metadata. It does not prove a working file still has those bytes. Verify reused data and completed file before publication.

Methods share mutex/OS-thread rules, change no pins and cannot cancel mid-native-call. Comparison JSON is bounded 128 MiB then decoded. Matching rebuilt headers/library are required; ABI1 gains additive symbols. See [ranges](doc:file-ranges) and [comparison](doc:file-comparison).

## Workspace observation {#workspace-observation}

`OpenWorkspace(project,storePath,WorkspaceOptions{})` arms the watcher and retains the full inventory without an open Store. Use `Status(store,WorkspaceSelection{})` or `Combined:true` for staging+working changes. Close the caller-opened Store after queries; `Wait(ctx,timeout)` returns typed native/polling hints. Batches remain retained until successful status, across waits/cancellation. `Invalidate` requests a complete audit; schedule those independently.

Zero options use Windows/Linux notifications with polling fallback,4096 hints,65536 registrations. Calls/Close serialize; close explicitly. Status returns canonical `json.RawMessage`. `OpenWorkspaceContext`/`StatusContext` pass cancellation to recursive registration/hashing; Wait interrupts native waits. Context can abandon serialization waits. Each native source is per-call and released only after callback shutdown. Completed native results win over late cancellation; OS IO/JSON may finish first. Go does not expose C progress callbacks. See [ownership and limits](doc:workspace-abi).

## Closing on Windows {#windows-test-cleanup}

Close Stores/sessions explicitly before removing their directories. The trusted DLL remains loaded for the process lifetime.

## Syncthing {#syncthing}

The optional [Syncthing module](doc:syncthing) adapts manifests to fixed SHA-256 BEP blocks using the pinned Syncthing v2.1.7 library. Authenticated device connections and differential transfers are tested against its real daemon. Syrnck objects and CDC retain their own format. Its native-backed catalog persists folder indexes, vectors, tombstones and verified data; staged downloads avoid holding Store across network waits. Workspace application, discovery, a pairing UI and complete autonomous scheduling remain unfinished.

## Registered workspaces {#registered-workspaces}

Open with `WorkspaceOptions{Name: "edit"}` and select `store.UseWorkspace("edit")`
before each status. Empty names use primary. Mismatched context fails; selection
failure leaves the prior name intact. See [linked contexts](doc:workspace-abi#registered-contexts).

## Native capture and recovery {#native-workspace-actions}

`workspace.NativeState(store)` reads the selected native context, reference,
revision, materialization and pending checkout. `state.Expected()` freezes the
exact target/revision/policy identity. Use it for `Capture(store, expected, message)`,
`PlanCheckout(store, expected, candidate)` and
`CheckoutOwned(store, expected, candidate, owner)`. Capture retains local changes;
checkout requires clean working files and preserves a recoverable owned journal.
`RecoverOwned(store, target, candidate, owner)` resumes only that exact pending
operation. All five methods have Context variants and enforce the session's
exact Store/context/directory binding. Mutation errors require re-reading state
because publication may already have occurred. See the [native action contract](doc:workspace-abi#native-workspace-actions).
