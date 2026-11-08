---
id: go-binding
title: Go binding
description: Go binding guide.
section: API
order: 10
parent: integrations
revision: 2
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
fmt.Printf("%d octets copiés\n", stats.Bytes)
return destination.Restore(id, "Map-restored.uasset")
```


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

The optional [Syncthing module](doc:syncthing) adapts manifests to BEP fixed SHA-256 blocks using Syncthingv2.1.7. It supports differential reconstruction and resumable staging with scanner/BEP TLS1.3 connections. Syrnck objects and CDC retain their own format. Resident daemon, discovery, conflicts and complete synchronization between Syncthing executables remain future work.

## Registered workspaces {#registered-workspaces}

Open with `WorkspaceOptions{Name: "edit"}` and select `store.UseWorkspace("edit")`
before each status. Empty names use primary. Mismatched context fails; selection
failure leaves the prior name intact. See [linked contexts](doc:workspace-abi#registered-contexts).
