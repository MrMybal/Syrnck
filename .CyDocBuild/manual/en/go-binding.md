---
id: go-binding
title: Go binding
description: Go binding guide.
section: API
order: 10
parent: integrations
revision: 14
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

The optional [Syncthing module](doc:syncthing) adapts manifests to fixed SHA-256 BEP blocks using the pinned Syncthing v2.1.7 library. Authenticated device connections and differential transfers are tested against its real daemon. Syrnck objects and CDC retain their own format. Its native-backed catalog persists folder indexes, vectors, tombstones and verified data; staged downloads avoid holding Store across network waits. The [continuous file controller](doc:syncthing#folder-controller) applies verified versions to native workspaces with owned recovery. Full directory/symlink semantics, copies for concurrent protocol versions, discovery, a pairing UI and OS service installation remain unfinished.

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

`CheckoutStatusContext(ctx, store, transaction)` reads one exact journal or
completed receipt with the core decoder. It verifies the bound Store/context/
directory. A committed receipt proves past application even if the user has
edited the working file since; it does not assert that the workspace is clean.

`RecoverOwnedPreserving[Context]` explicitly archives regular-file edits and
creates exclusive conflict copies before resuming the exact owned native journal.
Use the [active-edit contract](doc:workspace-abi#active-checkout-edits); ordinary
`RecoverOwned` remains strict, and every mutation error requires reloading state.

## Capture I/O report {#incremental-capture}

`CaptureReportContext` returns the revision and source read/reuse counters.
`CaptureContext` keeps its revision-only result. Session manifests can survive
Store reopens, with complete enumeration and object verification. See
[capture boundaries](doc:performance#incremental-capture).

## Native authorship and fresh revisions {#native-authorship}

`UseNativeIdentity(identity, seed)` selects a native author/committer for one
Store handle. An absent seed creates unsigned v2 metadata; a 32-byte seed selects
native Ed25519 signing. `NativeIdentity` contains name, email and timezone offset
in minutes east of UTC. Validation rejects invalid UTF-8, controls, excessive
lengths and offsets outside -1439..1439. Failed replacement keeps the old identity.

Use `ConfigureNativeIdentity(identity, NativeIdentityOptions{Seed: seed,
RequireSignature: true})` for a required signature. Without a supplied seed,
reading and recovery still work, but fresh native captures/preparations fail;
there is no silent unsigned fallback. `ClearNativeIdentity()` explicitly removes
this handle's identity/policy. `NativeIdentityMetadata()` returns public identity,
key identifiers and an enabled `signature_required` flag, never the seed.

`PrepareNativeRevisionContext(ctx, draft)` accepts bounded JSON for a fresh v1
revision. C++ canonicalizes it, verifies the complete parents/files, then applies
the handle's identity/signature. It rejects v2 input rather than replacing an
existing author. The unpinned returned revision needs guarded ref publication
before releasing Store to GC. It changes no refs or working files. Cancellation
covers closure verification; a late IO error may leave an unreferenced object
or the required-reader feature gate. This is distinct from raw `PutContext`,
which keeps its explicit low-level metadata contract.

The C API exposes matching `store_use_native_identity`,
`store_clear_native_identity`, `store_native_identity_json` and
`prepare_native_revision_ex` operations in the distributed header. No secret
pointer is retained. Native signing keys are wiped on replacement/clear/Close;
callers control, serialize and wipe their own seed buffers. Configurations and
backups do not acquire a private signing key from these APIs.

A resident provider must configure each newly opened Store before returning it.
Stop its consumers before retiring the provider. The Syncthing controller uses
this context for local captures and synthesized incoming native revisions;
[its standalone configuration](doc:syncthing#native-authorship) supports an explicit
author and a separate signing-key environment variable. Signatures are local
claims, not BEP device identity or server permission. Server publication still
uses its own approved-key/identity policy. OS vault integration remains separate.

## Protected native key owners {#protected-keys}

`ProtectKey(key, password, PasswordKdfOptions{})` returns a 104-byte authenticated envelope; an empty key generates a new random storage key. Input key/password buffers remain caller-owned. `ChangeKeyPassword` preserves the storage key and unspecified costs; old envelopes are not revoked. Zero fields select 64 MiB/three passes during protection and preserve existing costs during password changes.

`OpenProtectedKey(envelope, password)` authenticates once into a native owner. It exposes no plaintext export. Reuse `owner.Open(path, create)` for resident services and close the owner explicitly. Calls and Close are serialized; an owner must not be copied. Each Store retains its own key, so owner closure does not revoke or close existing Stores.

```go
func openProtectedStore(path string, envelope, password []byte) (*syrnck.Store, error) {
    owner, err := syrnck.OpenProtectedKey(envelope, password)
    if err != nil { return nil, err }
    defer owner.Close()
    return owner.Open(path, false)
}
```

`OpenProtected(path, envelope, password, create)` provides that one-shot convenience. For service callbacks, keep one owner alive until all service workers join, and open short Store sessions from it. Derivation is synchronous and bounded; passwords contain 1..4096 exact bytes, including NUL for SDK calls. Clear caller-owned input buffers when no longer needed. Authentication failure does not create a Store directory.

[CLI key files](doc:key-files) use the same envelope. Pair this binding with the matching native library; ABI 1 has additive key entry points.

## Protected signers {#protected-signers}

[Purpose-separated signing envelopes](doc:signing-keys) use `ProtectSigningKey`,
`ChangeSigningKeyPassword` and `OpenProtectedSigner`. The move-only native owner
is exposed as a non-copyable Go `SigningKey` with public metadata and explicit
Close. Use `NativeIdentityOptions.Signer` and `RequireSignature: true` to attach
independent state to short Store sessions without repeating Argon2id. Seed and
Signer are mutually exclusive. Closing an owner does not revoke a configured
Store or older envelopes; failed setup preserves the previous identity.

## Sealed repository owners {#sealed-repositories}

`ProtectSealedKey`, `ChangeSealedKeyPassword` and `OpenProtectedSealedKey` use the distinct 104-byte [sealed master-key envelope](doc:sealed-repositories#key-files). `SealedKey` keeps the unwrapped master in native memory with no plaintext export. `PublicMetadata` returns only version/type/public realm. Keep the owner alive until all operations finish, then Close explicitly. Input passwords/keys remain caller-owned; clear those buffers when no longer needed. An empty key/seed slice selects generation for all three protectors: storage, signing and sealed.

`SealSnapshot`, `InspectSnapshot` and `OpenSnapshot` accept a context, typed roots, an explicit channel and `SealedLimits`. Zero limits select core defaults. Sealing verifies the original closure and retains an opaque wire snapshot; inspection is read-only. Opening imports verified originals and atomically retains roots/checkpoint without changing native heads, staging or files. Existing checkpoints reject rollback/forks. A separate checkpoint permits explicit historical restoration.

The following function receives already opened, distinct source/wire/destination Stores and a verified original root. The wire path must be outside plaintext Stores, conventional project directories and registered workspaces. `previous` is an exact earlier sealed ID or empty for the initial snapshot.

```go
func restoreSealed(ctx context.Context, source, wire, destination *syrnck.Store,
    envelope, password []byte, root syrnck.ObjectKey, previous string,
) (syrnck.SealedSnapshotReport, error) {
    master, err := syrnck.OpenProtectedSealedKey(envelope, password)
    if err != nil { return syrnck.SealedSnapshotReport{}, err }
    defer master.Close()
    prepared, err := master.SealSnapshot(ctx, source, wire, "main",
        map[string]syrnck.ObjectKey{"heads/main": root}, previous, syrnck.SealedLimits{})
    if err != nil { return syrnck.SealedSnapshotReport{}, err }
    return master.OpenSnapshot(ctx, wire, destination, prepared.Snapshot, "main",
        "sealed/checkpoints/local-restore", syrnck.SealedLimits{})
}
```

Reports contain private original names/IDs and belong on the client. Transport only `ObjectKey{Kind: "revision", ID: report.Snapshot}` and its ordinary wire closure. The existing `CopyMissing` helper can exercise that opaque copy locally without a master key. Production resumable HTTP publication/fetch currently uses the C++/CLI [sealed transfer workflow](doc:sealed-repositories#remote); its remote-CAS/session wrappers are not part of this Go API. The native resident agent supports [sealed transport](doc:sealed-repositories#resident). Syncthing/BEP uses its [independent encrypted-device format](doc:syncthing#untrusted), including the [opaque receiver](doc:syncthing#opaque-receiver).

Go serializes the owner and all participating Stores, in consistent Store order, with context-aware lock waits. Concurrent Close waits for native use to finish. Context cancellation requests the native one-shot source and joins that request before release; successful publication wins a later stop request. An IO/output-allocation error can still follow publication, so reload refs and establish the outcome before retrying. These sealed operations do not add distributed key rotation/revocation or automatic encrypted-history retirement. System vault configuration is separate below.

## System key vault owners {#system-vaults}

`OpenSystemKeyVault("")` owns native provider configuration, not an unlocked OS session. An absolute constructor path selects a macOS file keychain only. `Provider()` reports compiled support; it does not prove that account credentials are accessible. [Platform requirements](doc:key-files#system-vault) apply to Windows Credential Manager, macOS file Keychain and Linux Secret Service. No error falls back to a raw/file/plaintext provider.

For macOS file keychains, a headless host must explicitly call `DisableMacOSKeychainInteraction()` once before Keychain workers. This changes **all Keychain interaction in the current process**, not the OS account or another application. It is a no-op on other platforms. Do not re-enable interaction while vaults are in use. Vault operations refuse missing setup without changing it; an interactive host must make that process-policy decision itself.

`Generate(VaultStorageKey|VaultSigningKey|VaultSealedKey)` creates a fresh random native key and public `VaultReference`. Persist `ref.String()` as local configuration; `ParseVaultReference` validates the exact version/purpose/lowercase ID. `ImportProtected` authenticates a matching 104-byte password envelope and creates a different reference. `ExportProtected` returns only an authenticated password envelope; retain and verify that independent backup outside working files.

```go
func openVaultStore(encoded, path string) (*syrnck.Store, error) {
    // The headless host chooses macOS process policy once, before workers.
    vault, err := syrnck.OpenSystemKeyVault("")
    if err != nil { return nil, err }
    defer vault.Close()
    ref, err := syrnck.ParseVaultReference(encoded)
    if err != nil { return nil, err }
    key, err := vault.OpenSecret(ref)
    if err != nil { return nil, err }
    defer key.Close()
    return key.Open(path, false)
}
```

`OpenSecret` accepts storage references, `OpenSigner` signing references and `OpenSealed` independent master references. Each returns the existing opaque native owner. Storage/seed/master bytes never enter Go. Configure a signer through `NativeIdentityOptions.Signer`; closing the provider or signer does not invalidate Store-owned signing state. A master works with `SealSnapshot`/`OpenSnapshot`. `Erase` removes the exact OS entry and returns false if absent; it does not revoke opened owners or exported backups. `Close` is idempotent and serialized with calls; later calls return `ErrSystemKeyVaultClosed`.

The standalone BEP configuration and native CLI environment do not yet select vault references. Library hosts can reuse the returned owner in `OpenStore` callbacks. The opt-in `key_vault_system` CTest fixture builds a Go probe with strict pointer checks and executes create/verify/erase in separate processes alongside the C++ and C shared/static probes. It checks real encrypted Store use, signed revisions, protected backups and sealed reconstruction; macOS also checks locked keychain refusal.
