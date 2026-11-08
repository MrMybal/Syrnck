---
id: syncthing
title: Syncthing fixed-block adapter
description: Syncthing fixed-block adapter guide.
section: API
order: 660
parent: integrations
revision: 5
---

# Syncthing fixed-block adapter {#syncthing-fixed-block-adapter}

This optional Go module connects the C++ engine to Syncthing's BEP block model.
It depends on the unmodified Syncthing library at **v2.1.7**, commit
`d6d0181f7d46`, represented by the Go pseudo-version pinned in `go.mod`/`go.sum`.
The core Go binding remains a separate module without external Go dependencies.
This module requires Go 1.26.2 or later; local validation used Go 1.27.3.

The adapter supports regular-file data, explicitly paired device connections,
a durable causal catalog and a continuous native-workspace controller.
`Folder` captures local changes, applies verified incoming versions and retains
recovery state. `FolderSession` runs that controller over authenticated BEP.
Discovery, a pairing UI, a standalone service executable and complete Syncthing
folder semantics remain work to complete; see the controller limits below.

## Native library and dependencies {#build-and-test}

Build `syrnck_ffi` and prepare the pinned Go dependencies once with `go mod download` from `bindings/syncthing`. The adapter is optional and separate from the core binding. Linux/macOS use cgo and their loader path. The earlier catalog and transport passed Linux CI; this new controller is locally qualified on Windows, with current cross-platform execution pending. Distribute the dependencies and notices required by the chosen integration.

## APIs {#apis}

`NewFile(ctx, store, manifest, folder, name, blockSize)` creates an immutable
servable view. It verifies the engine's complete file hash without exporting a
file, then reads verified ranges and constructs Syncthing `protocol.FileInfo`
SHA-256 descriptors. Engine CDC/fixed chunks and BLAKE3 object IDs stay intact.
A zero block size chooses Syncthing's own size policy; explicit sizes must be
powers of two from 128 KiB through 16 MiB. An index is capped at 65536 blocks.
Empty files use the reference implementation's single empty SHA-256 block.

`File.Info()` returns an independent index copy. Set application-owned version
vectors, sequence numbers and timestamps on that copy before announcing it.
Names must be valid UTF-8 relative paths. The adapter normalizes native Darwin
NFD names to NFC and converts native Windows separators to the protocol's `/`.
Indexes, block requests and resume identity use the same canonical name.

`File.ReadRequest(ctx, req)` serves an exact advertised block for the bound
folder and file. A supplied SHA-256 hash must match; the protocol's optional
hash and older zero/default block number are supported. A temporary-file
request can fall back to this complete immutable source. Other paths, ranges
or hash generations are refused. Payloads are verified by the engine and
checked against the announced SHA-256 digest before returning them.

`Fetch(ctx, destination, folder, info, request, options)` reconstructs one
announced file. Pass a `protocol.Connection.Request` method or another
authenticated block requester. Every response must have the exact size/hash.
An optional `ReuseManifest` indexes a local immutable file at the incoming
block size and reuses matching blocks, including moved/repeated blocks. Only
missing data is requested. `FetchResult.Stats` separates requested bytes, local
reuse and verified staging bytes.

```go
view, err := syncthing.NewFile(ctx, source, manifest, "project", "Content/Map.uasset", 0)
// Check err, authenticate the peer and announce view.Info() through its model.
result, err := syncthing.Fetch(ctx, destination, "project", incomingInfo,
    connection.Request, syncthing.FetchOptions{
        ReuseManifest: previousManifest,
        ResumeDirectory: perContentPrivateDirectory,
    })
// Check err before using result.Manifest or publishing a workspace revision.
```

After assembling and verifying private staging bytes, Fetch ingests the complete
file through the C ABI with fixed engine chunks at the BEP size. It rechecks the
stored index against the incoming block hashes. The engine retains the complete
manifest through its existing `ffi/ID` root. The embedding application can then
restore it with the engine's exclusive file publication or include it in its
own revision transaction. Fetch never replaces a working file or mutable
branch reference.

`syrnck_file_verify` / `Store.VerifyFile` are the additive C/Go ABI entry points
for targeted whole-file validation, including chunks, sizes and `file_hash`.
They write no destination or reference and stream through bounded native chunk
buffers. This binding requires a rebuilt native library exporting that symbol.

## Durable folder catalog {#folder-catalog}

`OpenCatalog(ctx, store, CatalogOptions)` loads a folder's durable catalog or
creates an unpublished empty draft. Bind the folder to the full local Device ID
and the exact trusted peer list. Duplicate devices, local/sentinel peers and
colliding 64-bit vector identities are refused. Membership changes require a
separate management workflow; changing the options cannot silently replace it.

A `Catalog` holds no Store or connection. `LocalIndex` and `PeerIndex` return
independent snapshots. `RecordLocal` or `RecordLocalChanges` assigns local
sequence numbers and advances the previous local version vector, with overflow
checks. Caller-supplied causal fields must be unset. Batch recording validates
all changes together and sorts names once. Deletions require explicit tombstones.
Directories and symlink descriptors are preserved as metadata; this API does
not create directories or follow symlink targets.

`ReceiveIndex(CatalogIndex)` records a paired device's full Index or delta. The
caller must bind `Device` to the authenticated connection, authorize the folder
and take `IndexID` from its negotiated ClusterConfig. A new generation requires
a full reset; subsequent batches must continue the persisted previous sequence.
Names and sequence numbers are unique, vectors are sorted/bounded and duplicate
counters, invalid flags, unsupported encrypted entries and malformed block
layouts are refused. Equal vectors cannot describe different content metadata.
Peer announcements do not automatically become local files or download data.

`Attach(ctx, store, device, name, manifest)` verifies downloaded data against the
currently recorded block descriptors. `Adopt(device, name)` promotes a verified,
causally newer peer entry into the local advertised index. Concurrent versions
return `ErrCatalogConflict`; older versions and equal-vector/content mismatches
are refused. Both versions remain retained, without a silent winner. This changes
the advertised catalog only; working-file application needs its own recovery
transaction before a service announces that a workspace has caught up.

`Commit(ctx, store)` verifies retained file data, then publishes state and data
under one expected-value guarded `ffi/bep/…` native revision root. Previous
catalog revisions are native parents, so an index reset, rename or deletion
keeps earlier complete versions reachable by GC and backups. Payload paths use
hashes of device/name identities; `.git/config`, case-distinct names and paths
incompatible with a portable workspace can remain lossless in storage. They
are not materialized by this API. No implicit `ffi/manifestID` pins are created.

A stale draft returns `ErrCatalogStale`. Reload and replan; do not retarget an
old operation. An IO error during publication has an unknown outcome: reload
and establish the persisted result before retrying. Unchanged commits are
no-ops. A fresh local IndexID becomes durable on the first successful commit.
The catalog codec rejects duplicate/unknown fields and noncanonical state;
limits are 256 paired devices, 256 counters/vector, 65536 entries across all
indexes and 16 MiB of catalog metadata. Each commit currently verifies its
retained files and native closure; large-project throughput is not established.
History retirement and catalog sharding remain future work.

## Download without holding the Store {#staged-download}

`DownloadFile(ctx, folder, info, request, DownloadOptions)` verifies network
blocks into private staging without opening a native Store. Optional `ReuseIndex`
and `ReadReuse` reuse exact matching local blocks; that callback can open/close
a Store briefly. Different block sizes disable reuse. Caller-owned resume data
uses the same validated descriptor and range checks as Fetch.

The returned `StagedFile` holds no open file or Store. Always `Close()` it.
`Info()` returns an independent copy and `Stats()` reports transfer/reuse/staging.
Call `Prepare(ctx, store)` while briefly owning the Store. It imports an unpinned
manifest and rechecks stored bytes against the announced blocks. Attach that
manifest to the exact current peer entry, optionally adopt it, then Commit
before closing Store or permitting GC. This prevents network waits from holding
the native exclusive lock. Revalidate the persisted peer entry after download:
it may have advanced while the transfer was in flight.

`PrepareFetch` is a convenience for a caller that already holds a Store; it has
the same unpinned ownership rule. Legacy `Fetch` continues retaining its result
independently. Keep these ownership choices explicit; do not remove a shared
legacy pin owned by another consumer.

## Authenticate a device connection {#device-connections}

`OpenDeviceConnection(ctx, stream, DeviceOptions, model)` takes ownership of an
unsecured connected `net.Conn`, including on failure. Supply one local TLS leaf
certificate with its matching `crypto.Signer` private key and the explicitly
paired peer's `protocol.DeviceID`. `Server: true` selects the accepting side.
Self connections, empty/sentinel identities, malformed keys and certificate/key
mismatches are rejected. Do not mutate the certificate or key while in use.

The connection requires TLS 1.3 or later and `bep/1.0` ALPN. It exchanges the
bounded upstream Hello before rejecting an unpaired peer, then verifies the
SHA-256 identity of the exact single peer certificate. This is certificate
pinning: DNS, a Web PKI CA, certificate expiry, discovery and Hello names do not
grant membership. `PeerHello` contains untrusted descriptive information.
No application model is attached until verification succeeds.

```go
session, err := syncthing.OpenDeviceConnection(ctx, stream, syncthing.DeviceOptions{
    Certificate: deviceCertificate,
    RemoteDevice: pairedDeviceID,
    Hello: protocol.Hello{DeviceName: "Asset workstation"},
}, folderModel)
// Handle err before use. Start is explicit; ClusterConfig must be the first
// post-authentication message. The model must authorize every shared folder.
session.Start()
session.ClusterConfig(clusterConfig, nil)
// Send a full Index before updates, unless a persisted delta-index agreement
// permits otherwise. Fetch can use session.Request for verified block data.
```

`HandshakeTimeout` defaults to 20 seconds and accepts 100 ms through one minute.
It covers TLS and Hello; an earlier context deadline wins. Compression defaults
to the reference metadata policy; Never/Always are also accepted. The context
controls the complete session lifetime, including an authenticated session that
has not been started. `Close(reason)` releases the stream; wait for the model's
`Closed` callback before closing native Store handles used by its callbacks.
The connection metadata contains a stable establishment time and session ID.

The application supplies certificate storage/rotation, trusted Device IDs and
connection scheduling. The low-level connection accepts an application model;
`FolderSession` supplies the regular-file controller described below. Transport encryption authenticates and protects this peer
connection; it does not make an ordinary receiving daemon an untrusted encrypted
backup. The complete untrusted-device encryption workflow remains to be integrated.

## Qualify against the reference daemon {#reference-daemon}

```text
python scripts/build_syncthing_reference.py --download
```

This optional test helper compiles `cmd/syncthing` from the exact adapter module
version using the checked-in `tests/syncthing-reference/go.mod` and `go.sum`.
It uses the upstream `noassets` build tag, which omits the web UI assets while
retaining the real daemon, database, scanner, model, puller and REST API.
The output is `build/qualification/syncthing` (`.exe` on Windows). Without
`--download`, the helper requires a prepared module cache and makes no implicit
module download. It verifies the executable's embedded upstream module version
and refuses adapter/reference version mismatches or modified manifests. It also
resolves the adapter test dependency graph with its platform cgo setting before
offline tests, including Darwin’s native watcher dependency.

Set `SYRNCK_SYNCTHING_BINARY` to that executable's absolute path and run:

```text
python scripts/test_go.py --library build/Release/syrnck_ffi.dll --module syncthing --check-pointers
```

On Linux/macOS provide the built shared library path instead. The three-platform
CI enables the adapter and this reference test explicitly. The reference daemon
test uses fresh test-owned configuration, keys and folder storage, binds only
loopback, disables discovery/relays/NAT/upgrades/reporting and terminates its
process after the test. It never reads or changes an installed Syncthing profile.

Local Windows verification exercised both directions against the unmodified
upstream daemon: a 384 KiB asset with moved/reused blocks transferred one missing
128 KiB block in each direction. Reconstruction into encrypted Syrnck storage
verified the resulting complete file. Real daemon rename/tombstone announcements
were also received. This proves transport and regular-file interoperability;
a further real-daemon test persists full/delta indexes and data, closes native
Stores during network waits, then reconnects after an offline rename and
preserves its tombstone. The production-controller test additionally reconstructs an actual native
working folder, returns a post-completion local edit to the daemon, reconnects after
an offline rename and propagates a local deletion upstream. Without the explicit
binary, live-daemon tests report a skip; ordinary adapter tests still run.

## Resume and cancellation {#resume-and-cancellation}

Without `ResumeDirectory`, Fetch uses a private temporary file and removes it
when the call returns. With it, the caller owns a per-content staging directory
containing `state.json` and `data`. Metadata binds the folder/name/size/block
size and the hash of the complete announced block list. A different index,
unknown file, incomplete descriptor or symlink is refused. Use a private
directory and serialize callers managing it.

On retry, every staged range is hashed again. Complete matching ranges are
reused; partial/corrupt ranges are fetched again. Sparse zero-filled ranges can
satisfy matching zero blocks without a request. Progress is flushed every
16 written blocks and before ingestion. Cancellation preserves the caller-owned
staging data. The staging file contains plaintext, including when the Store is
encrypted; choose an appropriately protected location. Completed staging
directories remain caller-owned and require explicit retirement.

Context cancellation is checked between native calls and network/block steps.
Unpinned preparation propagates cancellation into the native ingestion
operation. Legacy retained ingest and complete verification finish their current
synchronous native call.
A late cancellation or a detected change during ingestion can therefore leave
a complete internally retained manifest; its ID is returned alongside the
error. Callers must handle the error before publishing that result. No partial
working file is installed. A process kill can leave private temporary data;
power-loss durability and automatic staging reclamation are not established.

## Continuous working-folder controller {#folder-controller}

Create the native Store and workspace first, then call `NewFolder(ctx, options)`.
`FolderOptions` binds one catalog/local device, its explicit peer list, the
working directory, Store and optional registered workspace name. `OpenStore`
returns a newly opened handle owned by the controller: an encrypted integration
can obtain its key from the embedding application's provider. No idle Store is
retained. Close sessions before closing Folder.

`Refresh` captures local working edits using the native watcher/index and
periodic full audits (five minutes by default). `Invalidate` requests an immediate
full audit. `ReceiveIndex` persists an authenticated paired device's full/delta
index. `Step` downloads one missing file, reuses verified blocks across current
local files, then applies the complete candidate through the native owned
checkout. Exact-content renames need no payload transfer. Immutable advertised
bytes stay servable while a new version downloads; incoming indexes can advance
on a separate worker. Every downloaded descriptor/generation is rechecked before
attachment, so stale downloads cannot be installed.

Incoming application retains three typed roots for its descriptor, native
candidate and prepared catalog before touching working files. Protocol publication
waits for completed native evidence with this operation's unique durable owner,
source, target and selected branch. `CheckoutStatusContext` uses the core receipt
decoder, without reconstructing a private record format in Go or diffing entire
historical asset trees. HEAD equality alone is insufficient. A local edit after
native completion is captured with the received causal base; it cannot deadlock
the acknowledgement or be overwritten. A preparation obstructed before any
native journal starts is retired only after checking the exact original native
state. Foreign/ambiguous journals remain explicit.

The catalog retains previous complete versions through ordinary native revision
parents; its pending candidates are also GC roots. Process-termination tests kill
a real child before and after native publication, run GC, reopen, recover and
verify the incoming bytes or a subsequent local edit, with plain/encrypted
Stores. Concurrent protocol versions currently block; automatic conflict copies
are not yet implemented by this BEP controller.

## Continuous authenticated sessions {#folder-sessions}

`OpenFolderSession(ctx, folder, stream, options)` owns the connected transport,
checks that the certificate matches the catalog's complete local Device ID,
authenticates the configured paired peer, then negotiates folder generations and
full/delta indexes. `PollInterval` defaults to 250 ms (50 ms through one minute).
The incoming queue defaults to 32 MiB (1 through 128 MiB), includes the batch
being processed and never blocks the BEP reader waiting for native disk work.
`Reports`, `Errors` and `Done` expose progress, blocked operations and shutdown.
`Close` stops workers and waits for the transport/model shutdown.

The embedding application still owns listening/dialing, reconnect backoff,
certificate/key storage and OS service lifecycle. This module is separate from
the CMake engine distribution. The controller currently applies regular files
and file tombstones, not directory/symlink metadata, empty-directory lifecycle,
platform ownership/xattrs or receive-encrypted folders. It retains incoming
metadata but does not claim these paths have been applied. It also lacks
conflict-copy reconciliation, persistent download resume, automatic membership
changes/history retirement and authored/signature policy for newly synthesized
incoming native candidates. Large-project performance and power-loss durability
are not qualified. The native resident publication agent is a separate feature.

`StagedFile.PrepareWithChunking(ctx, store, syrnck.Chunking{})` keeps default
FastCDC native chunks independent of BEP SHA-256 boundaries. Legacy `Prepare`
keeps its fixed-BEP chunking. `Catalog.CommitWithRefs` atomically publishes its
catalog with additional typed reference guards; callers must reload after an
unknown publication outcome, as with the native CAS.

An external writer can change a file while a native journal is still actively
installing/verifying it. Native recovery then blocks rather than overwriting
that work. Preserve the changed data and resolve that journal explicitly;
automatic preservation/reconciliation during active application remains work
to complete. File visibility alone is not a completion notification: use the
controller checkpoint/status. This differs from the supported edit after the
native receipt has committed, including before its protocol acknowledgement.

The controller maps native filename spelling to canonical NFC protocol names.
It preserves an existing decomposed native spelling when applying edits and
rejects two native names that normalize to the same protocol path. An echo or
rescan cannot turn that spelling difference into a deletion.
