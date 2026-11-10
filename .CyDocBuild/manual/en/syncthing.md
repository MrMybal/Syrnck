---
id: syncthing
title: Syncthing fixed-block adapter
description: Syncthing fixed-block adapter guide.
section: API
order: 660
parent: integrations
revision: 16
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
Discovery, a pairing UI, OS service installation and complete Syncthing
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
backup. The optional [untrusted-device workflow](doc:syncthing#untrusted) adds end-to-end encryption for a receive-encrypted peer.

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
Stores. Concurrent protocol versions block by default; the explicit
[conflict-copy policy](doc:syncthing#protocol-conflicts) retains both contents.

## Persistent controller downloads {#persistent-downloads}

`FolderOptions.ResumeDownloads: true` enables reuse of verified private ranges
across connection loss and controller/process restart. The standalone equivalent
is `"resume_downloads": true`; the default is false. An embedding must supply an
explicit Store directory. The optional path lives below
`STORE/bep-downloads/v1/`, grouped by folder/workspace identity, with a separate
hash for the pinned peer, index generation and exact announced content index.
A changed generation/content index selects another path.

An operating-system file lock serializes staging for that folder across
controllers, without opening a native Store during network waits. Lock
contention reports `ErrFolderDownloadBusy`; process termination releases the
lock. The lock file remains in place to preserve its identity. Windows, Linux
and macOS have lock implementations; other unsupported targets refuse this
option. Do not remove or edit the private staging/lock paths while running.

Every saved range is rehashed against the announced SHA-256 before reuse.
Incomplete or corrupt blocks are requested again. Import verifies the complete
stream. Attachment requires the current peer generation and exact announced
metadata before retaining the immutable manifest. Stale downloads cannot be
installed. A complete matching staging pair is removed only after successful
catalog retention; native working-file application still uses its owned
transaction and receipt.

Persistent staging is **plaintext even when Store objects are encrypted**. Keep
the Store directory private and enable the option deliberately. The example
leaves it disabled. Core backups do not include these private unretained ranges.
Failed/obsolete staging is kept for retry or inspection; automated age/quota
retirement remains unfinished. After stopping all controllers, such directories
can be removed; they are not history roots. Unknown entries or mismatched
metadata are refused, never recursively deleted. An interrupted cleanup can
leave an incomplete private directory requiring inspection.

Tests exercise reopen, corrupted-range rejection, exclusive lock contention,
changed peer generation and stale announcements. Actual child-process kills in
plain/encrypted Stores retain the first verified block and request only the
remaining blocks after restart. Reference-daemon standalone tests enable this
option with both-direction changes, offline rename and conflicts. This does not
establish power-loss durability or protection from arbitrary external writers.

## Concurrent protocol versions {#protocol-conflicts}

Set `FolderOptions.ConflictCopies: true`, or `"conflict_copies": true` in the
standalone configuration, to resolve concurrent regular-file vectors. The
strict default still returns `ErrCatalogConflict`; `Catalog.Adopt` keeps that
behavior. Embedders can explicitly call `Catalog.ResolveConcurrent(ctx, store,
peer, name)` on a draft, then publish it with the normal verified transaction.
Its result describes the draft choice, not a working-file receipt.

Both live inputs are checked against their announced BEP blocks before choosing
a winner. The pinned upstream `FileInfo.WinsConflict` selects the newer modified
time, then orders concurrent vectors on a tie. Locally captured versions use the
native capture timestamp. The winner keeps its metadata and receives the union
of both vectors. Concurrent equal contents coalesce by the native whole-file
hash, independently of chunk boundaries; two tombstones also coalesce.

Different losing contents become a tracked sibling containing `.sync-conflict-`,
with a deterministic origin/vector/content identity and the short extension
preserved. A losing deletion has no contents to copy; a winning deletion keeps
the losing live file in a copy. Copy names reserve existing tracked names and
tombstones, including Unicode case/normalization and file/directory prefix
collisions. Bounded alternate names avoid replacing an occupied tracked path.
Other native namespace or working-file obstructions still block application.
Copies follow the workspace's materialization selection; hidden copies remain
retained logical files. There is no text or binary content merge.

The complete winner, copy and causal choices are frozen in the existing native
pending transaction. Recovery applies that candidate even if the option is
subsequently disabled. Only its owned completion receipt permits catalog
acknowledgement. Both inputs remain reachable through catalog history; copies
are ordinary tracked files and are not automatically retired. Syncthing may
create its own conflict-copy names when it resolves first.

Tests cover symmetric two-controller convergence without repeated history,
equal-content coalescing, live/deleted combinations, validation failures and
Unicode collisions. Actual process termination before/after publication, GC,
verified backup/restore and recovery preserve both inputs in plain/encrypted
Stores. Standalone processes also retain the losing contents on both devices
against the unmodified reference daemon, with each side winning once.

## Continuous authenticated sessions {#folder-sessions}

`OpenFolderSession(ctx, folder, stream, options)` owns the connected transport,
checks that the certificate matches the catalog's complete local Device ID,
authenticates the configured paired peer, then negotiates folder generations and
full/delta indexes. `PollInterval` defaults to 250 ms (50 ms through one minute).
The incoming queue defaults to 32 MiB (1 through 128 MiB), includes the batch
being processed and never blocks the BEP reader waiting for native disk work.
`Reports`, `Errors` and `Done` expose progress, blocked operations and shutdown.
`Close` stops workers and waits for the transport/model shutdown.

The embedding application supplies listening/dialing, certificate/key storage
and OS lifecycle. The [resident service](doc:syncthing#folder-service) provides
reconnect backoff and independent offline capture. This module is optional; the CMake BEP target packages the standalone executable. The controller currently applies regular files
and file tombstones, not directory/symlink metadata, empty-directory lifecycle,
platform ownership/xattrs. The separate [opaque receiver](doc:syncthing#opaque-receiver) retains encrypted metadata without applying it to a workspace. The controller retains incoming
metadata but does not claim these paths have been applied. It also lacks
automatic membership
changes/history retirement. Large-project performance and power-loss durability
are not qualified. The native resident publication agent is a separate feature.

`StagedFile.PrepareWithChunking(ctx, store, syrnck.Chunking{})` keeps default
FastCDC native chunks independent of BEP SHA-256 boundaries. Legacy `Prepare`
keeps its fixed-BEP chunking. `Catalog.CommitWithRefs` atomically publishes its
catalog with additional typed reference guards; callers must reload after an
unknown publication outcome, as with the native CAS.

An external writer can change a file during active native installation. Default
recovery stays strict. Set `FolderOptions.PreserveCheckoutEdits = true` to retain
regular-file edits as described below. File visibility alone is not a completion
notification: use the controller checkpoint/status. An edit after the native
receipt has committed is supported independently of this option.

The controller maps native filename spelling to canonical NFC protocol names.
It preserves an existing decomposed native spelling when applying edits and
rejects two native names that normalize to the same protocol path. An echo or
rescan cannot turn that spelling difference into a deletion.

## Preserve edits during active checkout {#active-checkout-edits}

The explicit `PreserveCheckoutEdits` option is frozen in each prepared
application; changing future configuration cannot retarget an interrupted
operation. Recovery checks the exact owner, native target, context, directory
and private staged/backup files. It ingests changed regular files into native
archive revisions before moving any working bytes. Exclusive conflict-copy
moves and exclusive restoration of the expected intermediate bytes are replayed
from retained descriptors. The original strict checkout then rechecks all target
files and publishes its unchanged candidate and receipt.

Archive roots use `archive/checkout-edits/TRANSACTION/EDIT/state` (descriptor)
and `/work` (revision). Their native parents retain the incoming revision and
its complete file closure. Additional `archive/checkout-edit-versions/TRANSACTION/REVISION` roots retain changes
to a conflict copy during recovery. Copies are deterministic from each retained
edit identity, preserve short extensions and are never overwritten. A file that
appears at a copy name is kept; remaining original edits get a fresh identity.
The report's `preserved_edits` lists source paths, copy paths and archived
revision IDs; a copy's current bytes may be newer than that archived revision.

After acknowledgement, normal local capture advertises the preserved files.
This protects file-content edits before backup, during staging and after file
installation, including an otherwise unchanged tracked file. Prepared signed
candidates remain unchanged; recovery needs no signing private key. Archives
remain GC roots after workspace changes. Automatic retirement of these groups
still needs an administrative policy; do not remove their roots independently.

An actively changing editor can defer completion: each bounded attempt keeps
its observations for the next retry. Missing-file/deletion intent, directory or
symlink obstructions and ambiguous namespace changes still block rather than
inventing a resolution. This option does not resolve concurrent BEP version
vectors, implement a complete folder service or establish power-loss safety.

If the original checkout must remove or rename a parent directory, preservation
places the copy outside that directory while keeping the source basename and
short extension. This also covers directory-to-file and directory case changes.

## Resident reconnection service {#folder-service}

`OpenFolderService(ctx, folder, options)` runs one explicitly paired peer across
connection loss. Supply `FolderServiceOptions.Session` with the device certificate,
paired remote ID and session budgets, plus `Connect(ctx)` returning a connected
`net.Conn`. The factory can dial or accept, must honor cancellation, and transfers
ownership of any returned connection, including alongside an error. TLS/BEP peer
authentication always runs after the factory; it cannot authorize a device.

The service observes/captures local changes independently of connection progress,
including offline edits, deletion/rename and an authenticated peer that never
sends its folder configuration. `OfflineInterval` defaults to 250 ms (50 ms
through one minute). It also runs during downloads: the native folder controller
serializes state and rejects stale candidates without holding Store during
network waits. Ordinary full audit scheduling remains enabled.

`RetryMin` defaults to 500 ms and `RetryMax` to 30 seconds. Failed connections use
exponential backoff within configured bounds (50 ms through one minute); a
successful authenticated connection resets it. Each reconnect sends a fresh
Hello timestamp by default and resumes durable native/catalog state. No discovery
or implicit pairing is introduced.

`Events` reports connecting, connected and retrying; connected means certificate
authentication, not completed folder application. `Reports` and `Errors` expose
folder progress and failures. All three bounded streams are advisory and may drop
messages under consumer backpressure; reload durable state for receipts. `Close`
cancels/joins dialing, TLS and session workers, including a transport completing
after cancellation, then closes streams and `Done`. Stop the service before
closing its caller-owned Folder. It keeps no idle Store. Storage-key lifetime
belongs to the Folder’s application-supplied Store provider; keep the device
signing key immutable until shutdown. Close returns any session teardown error.

Tests cover plain/encrypted two-workspace offline capture, TCP reconnection,
offline rename/tombstone convergence, failed factory transport cleanup, stalled
TLS cancellation and local capture while an authenticated peer withholds folder
configuration. Existing session tests separately qualify the pinned upstream
daemon. The application still supplies network listening/dialing, certificates,
key storage and OS lifecycle. OS service packaging and
complete directory/symlink semantics remain unfinished.

## Native author and signing policy {#native-authorship}

Configure every Store returned by `FolderOptions.OpenStore` with the native
[identity API](doc:go-binding#native-authorship). Local captures and synthesized
incoming workspace revisions then use that author/committer and optional signer.
An incoming BEP version becomes a local native integration revision; it does not
claim the peer's native author identity. Catalog bookkeeping remains internal v1
metadata. TLS device keys, storage keys and native signing seeds have separate
roles. This does not import remote native signatures through BEP file vectors.

The standalone JSON accepts optional `author` with `name`, `email` and
`timezone_offset` (minutes east of UTC). Without author, legacy v1 behavior stays.
With author but no `signing_key_env`, captures/candidates are unsigned v2. Set
`signing_key_env` to a separate uppercase environment variable containing a
32-byte Ed25519 seed as 64 hexadecimal characters to require signatures. No raw
seed belongs in JSON or process arguments. Each short-lived Store receives a
native key copy; decoded Go seed buffers are cleared immediately afterward.

An unset configured signing variable allows public recovery of an already
prepared signed candidate, then blocks fresh revisions until the seed returns.
A malformed seed is an error. Working edits remain in place when signing fails.
Prepared candidates keep their exact signed bytes, parents and native receipt
guards; they are never re-signed during recovery. Clearing the requirement in the
configuration is an explicit policy change. OS vaults, secret distribution and
signer authorization remain external; a valid signature is not a server grant.

Tests kill signed incoming applications before/after native publication in
plain/encrypted Stores, recover without the private key, reject unsigned fallback
for a subsequent edit and resume signing after restoring the provider. Standalone
process restart and live reference-daemon tests qualify signed local captures
and incoming native revisions. The reference daemon itself does not sign native
Syrnck revisions.

## Standalone foreground executable {#standalone-service}

The optional `syrnck-bep` executable runs the native-backed resident service for
one existing workspace and one explicitly paired peer. It captures offline
changes, authenticates the peer, reconnects after failures and applies guarded
regular-file versions. It uses no Git process and requires no Go installation at
runtime. The C++ engine remains the storage/revision implementation.

```text
syrnck-bep device-id --certificate device/cert.pem
syrnck-bep run --config service.json
syrnck-bep --lang fr run --config service.json
```

Provision a distinct device certificate/private key externally and configure its
printed full Device ID on the peer. A Syncthing `generate` identity can be used;
reuse its certificate and matching key together. Self-connections are refused.
The CLI imports identities; generation, rotation and OS vault integration remain
separate work. Both sides must share the same folder ID.

Copy the installed `share/Syrnck/examples/bep-service.json` into your configuration
directory and replace its paths, peer ID and address. The example is also in the
source `examples` directory. Initialize the workspace Store with the main
`syrnck` CLI first. Relative paths resolve against the configuration file. The
Store defaults to `directory/.syrnck`; `workspace` can select an existing linked
context. `mode: dial` connects to `address`; `listen` accepts there instead. An
explicit `host:port` is required. Each process owns its transport and Folder.

The PEM private key must be outside the working tree or inside its protected
Store, including after resolving links. It must not become source content.
`key_env` names the storage key environment variable (default `SYRNCK_KEY`):
absent means no supplied key; otherwise its value must be 64 hexadecimal
characters matching the existing Store. No raw key belongs in JSON or arguments.
TLS identity and at-rest encryption keys have different roles. The PEM file stays
under its filesystem permissions; Store encryption protects stored objects.

Alternatively, supply a [password-protected key file](doc:key-files) with
`"key_file": "storage.syrkey"` and optional `"key_password_env": "SYRNCK_KEY_PASSWORD"`.
Relative paths resolve against the JSON configuration directory. Its password
variable must contain 1..4096 exact bytes (UTF-8 from the Windows environment),
and the configured raw `key_env` must be empty. No password belongs in JSON or
arguments. The file must be a regular, non-linked 104-byte envelope outside the
working tree or inside the protected Store. Authentication failure stops before
capture/listening; it never falls back to an unencrypted Store. The native key
owner is reused until all service workers stop, without exporting its plaintext
key to Go. Changed envelopes/passwords require a restart; rewrap the same key to
a new file and update configuration/credentials before restarting. Running
services keep their already authenticated key. This does not revoke old envelopes.

Configuration version 1 rejects duplicate/unknown fields and inputs over 64 KiB.
`poll_ms` is 50..60000 (zero selects 250); retry bounds default to 500 and 30000 ms.
`use_permissions`, `preserve_checkout_edits` and `conflict_copies` default false.
The example explicitly enables [active-file preservation](doc:syncthing#active-checkout-edits)
and [concurrent-version copies](doc:syncthing#protocol-conflicts).
`resume_downloads` also defaults false; read the [plaintext staging requirements](doc:syncthing#persistent-downloads) before enabling it. These protect
different situations and can be configured independently.

Stop with Ctrl+C or SIGTERM. Restart with the same identity/configuration to
resume retained native/catalog state. Standard output is canonical JSON;
`ready` means local initialization/listener setup, not remote synchronization.
Human help and CLI-owned diagnostics use English/French; `--lang` overrides
`SYRNCK_LANG`, then English. Technical OS/library details retain their language.
The main CLI opens this page with `syrnck help syncthing --section standalone-service`.
OS service installation, multiple-peer orchestration and full directory/symlink
semantics remain unfinished.


For an OS-account provider, use `key_vault` and optional `signing_key_vault` instead of password files. See [system vault service settings](doc:system-vaults#services) for exact reference purposes, exclusions, macOS keychain/trust requirements and backup/erasure semantics. These settings do not move the TLS device PEM or Syncthing folder password into a vault.

## Build and distribute the executable {#build-standalone-service}

Enable `-DSYRNCK_BUILD_BEP_SERVICE=ON` in CMake (default on when Syncthing tests
are enabled). Build `syrnck_ffi` first. With Go 1.26.2+ available, prepare the
pinned public dependencies once:

```text
python scripts/build_bep_service.py --download --library NATIVE_LIBRARY --output BEP_EXECUTABLE
```

Use `build/Release/syrnck_ffi.dll` / `build/Release/syrnck-bep.exe` on the Windows
preset, `build/libsyrnck_ffi.so` / `build/syrnck-bep` on Linux, and
`build/libsyrnck_ffi.dylib` / `build/syrnck-bep` on macOS. Then build normally;
CMake invokes the same helper offline. Unix requires cgo and the matching native
library. This target uses a separate nonsanitized build from core ASan checks.

`cmake --install build --config Release --prefix dist` installs the executable,
matching native library and local manual. It also retains the actual runtime Go
modules’ unmodified source archives, licenses/notices, Go runtime license and
hash manifest under `share/licenses/Syrnck/bep`. Keep them with the distribution;
the project MIT license does not relicense dependencies.

Local Windows tests run actual standalone processes against the pinned upstream
daemon in plaintext/encrypted Stores, with both-direction edits and an upstream
offline rename across process restart. Separate hard-termination tests reconstruct
both old and new native versions. Linux/macOS executable and relocation checks
remain subject to their CI results; the source configuration alone is not proof.

The standalone service also supports [protected native signing keys](doc:signing-keys#services) through `signing_key_file` and `signing_key_password_env`. Authentication precedes workers/listeners and uses a typed native owner shared across short Store sessions.

## Untrusted encrypted devices {#untrusted}

The trusted Syrnck endpoint can exchange encrypted files with a real Syncthing **Receive Encrypted** daemon. It uses the pinned upstream codec and password-token negotiation, without modifying Syncthing or implementing a second crypto format. This is distinct from [sealed Syrnck revision snapshots](doc:sealed-repositories): it carries Syncthing file metadata/blocks, not the native commit graph. The separate [opaque receiver](doc:syncthing#opaque-receiver) provides the receive-encrypted role in Syrnck.

Copy the installed `share/Syrnck/examples/bep-untrusted-service.json`, or add these settings to an existing standalone configuration:

```json
{
  "untrusted_peer": true,
  "folder_password_env": "SYRNCK_BEP_FOLDER_PASSWORD"
}
```

Set that variable through a trusted launcher to the same nonempty folder password on each trusted endpoint. Configure the peer's folder as `receiveencrypted`; use the same folder ID. The CLI reads 1..4096 UTF-8 bytes once before capture/listening and removes the variable from its own environment on success or error. It never falls back when the configured password is absent. The provider name must differ from storage/signing/other key providers. Passwords do not belong in JSON or arguments. The launching process is unchanged. This secret is separate from a Store key, signing seed, device certificate or sealed master; it is not opened by the native sealed-key API.

Library callers set `FolderSessionOptions{Untrusted: true, EncryptionPassword: password}` for a session or its service. Leave both fields unset for ordinary trusted-peer transfer. `FolderService` reuses its codec/key derivation across reconnects, including recovery from temporary upstream enrollment failures. A direct `FolderSession` caller owns its reconnect loop. The upstream Go implementation retains password strings and derived-key caches; it does not provide the native key owners' explicit memory-wiping mechanisms. Protect the trusted process and use a strong independent password. A matching public token is required before authorizing indexes or requests; missing, wrong or conflicting tokens are refused. Initial empty configurations from a new upstream encrypted folder are allowed while it learns the token, with no data authorization. Authorization waits until local codec keys are installed.

The endpoint stores a canonical peer/folder encryption policy and a hash of its password token in an immutable chunk referenced by `ffi/bep/…/workspace/encryption/PEER_HASH`. Startup binds the selected mode/password before opening the connection, even if the peer is offline. Reconnects are idempotent. An explicit initial plaintext-to-encrypted upgrade is allowed; after encryption is selected, removing it or changing the password is refused before dialing. Preserve this checkpoint in backups. Correct a provisioning mistake through explicit migration/reprovisioning; deleting a catalog/checkpoint to bypass it is not automatic key rotation. Distributed password rotation/revocation and protection against an older application that ignores this policy are not provided.


For a password change or provisioning correction, stop the affected services, retain a verified Store backup, then provision a new shared folder ID and matching independent password on the trusted endpoints and a fresh receive-encrypted folder. The native Store/history can be retained; the new folder ID creates a separate causal/encryption context and uploads its current data again. Verify restoration before explicitly retiring the old remote folder. This does not revoke old password/data copies. Enabling encryption cannot hide data already sent to a previously trusted peer.

Encrypted indexes are authenticated/decrypted before entering the causal catalog; tampered or plaintext metadata closes the connection. Block responses are decrypted/authenticated by the upstream codec and then verified against the original SHA-256 descriptors before native ingestion. Short final and empty blocks use Syncthing's padding, while the engine reads only the exact original advertised range. Existing native history, guarded application, conflict copies and restart recovery continue to apply. Resume staging and working files remain plaintext on the trusted endpoint under their existing protection rules.

The untrusted device sees the folder ID/label, approximate sizes, update timing, encrypted history and equality at the same filename/offset. Paths, file metadata and contents remain encrypted. Syncthing derives file keys from the filename and encrypts block-hash tokens with their offsets: changing a filename or moving content to another offset can require retransmission. An unchanged block at the same offset can be reused. Refer to the [upstream protocol](https://docs.syncthing.net/specs/untrusted.html) and [user guide](https://docs.syncthing.net/users/untrusted.html) for its threat model and current upstream status. Authentication is not proof that a withholding peer is current; retain trusted causal state. Initial-root trust/gossip and distributed retention remain separate work.

Windows qualification uses the unmodified pinned daemon: a trusted source, an independent receiver, a same-offset block delta, reconnects, then a fresh trusted device restoring with both previous clients disconnected. It also checks empty/short-tail blocks, opaque on-disk names/data, actual standalone process restart, token disagreement, persisted downgrade refusal and plaintext/tampered metadata over pinned TLS. The synthetic 384 KiB file test changes one 128 KiB block; measured protocol output is approximately 129 KiB. This is not an Unreal-scale throughput/RSS claim. Current Linux/macOS execution still depends on CI results.


## Opaque encrypted receiver {#opaque-receiver}

`EncryptedReceiver` stores the upstream encrypted file metadata and ciphertext blocks directly in native Syrnck storage. It has no folder password, scanner or working directory. Trusted peers hold the folder password; the receiver binds their public password token to its durable folder catalog before accepting data. Certificate pinning still determines membership. A changed token or plaintext/mixed role is refused. The first explicitly paired peer provisions this token; this is not independent first-contact trust or distributed key rotation.

Use `NewEncryptedReceiver(ctx, EncryptedReceiverOptions{Catalog: catalog, Storage: storage})` with an initialized Store. An `OpenStore` provider can instead open short-lived Stores with a separate at-rest key. `OpenEncryptedReceiverSession` authenticates one paired transport; `OpenEncryptedReceiverService` adds reconnect/backoff and joined shutdown. Sessions return the same bounded `FolderSession` reports/errors. Several explicit peer services may share one receiver. Close all sessions/services before the receiver. No native Store remains open while awaiting network blocks.

The standalone executable supports one configured peer in this role. Copy the installed `share/Syrnck/examples/bep-encrypted-receiver.json` and replace the identity, paths and peer ID. For example:

```json
{
  "version": 1,
  "receive_encrypted": true,
  "storage": "opaque-store",
  "folder": "project",
  "certificate": "device-cert.pem",
  "private_key": "device-key.pem",
  "peer": "REPLACE_WITH_EXPLICIT_PAIRED_DEVICE_ID",
  "mode": "listen",
  "address": "127.0.0.1:22001"
}
```

Initialize `storage` with the ordinary native CLI before starting the service. Paths resolve against the JSON file. Configure the trusted counterpart with its folder password for this receiver; Syrnck trusted clients use `untrusted_peer: true`. The receiver must omit `directory`, `workspace`, `untrusted_peer`, `folder_password_env`, author/signing fields and working-file policies. It cannot decrypt or make authored plaintext revisions. Optional storage `key_env`/`key_file` providers still protect its native physical objects independently. The `ready` event includes `receive_encrypted: true`; it is not a synchronization receipt.

A distinct `ffi/bep-encrypted/…` catalog retains the public folder token, peer generations/indexes, original encrypted envelopes and complete per-block native manifest bindings. Completed block receipts under `ffi/bep-receipts/…` retain interrupted downloads through process restart and GC. Resume requests only missing token-bound blocks. Once a complete file is retained by the parent-linked catalog history, exact receipt roots are removed with a catalog-guarded compare-and-swap. Superseded incomplete receipts remain retained; automatic pruning is not implemented. Keep these references/history in backups, and never delete them solely because a working filename disappeared.

The receiver compares opaque 48-byte block tokens; these are not hashes it can verify against plaintext. Native hashes verify stored ciphertext integrity; trusted clients authenticate/decrypt and verify the original data. Ciphertext for unchanged name/offset/token/size is reused. Maximum 16 MiB encrypted BEP blocks include 40 extra bytes and are split across two native manifests to preserve the native 16 MiB buffered-operation bound. Only complete files are advertised. Encrypted directory/symlink metadata is retained and forwarded without being materialized; trusted workspace application still has its documented metadata limits. No standard Syncthing encrypted filesystem tree/trailer export is produced.

Opaque clocks conceal the private causal vectors. All peer versions and previous catalog roots remain retained; the advertised version uses the greatest opaque counter and a stable source-device tie break. This does not resolve plaintext conflicts. Trusted receivers decrypt the real vector and apply their existing conflict policy. There is no server-side proof that ciphertext from an authorized malicious peer decrypts, no freshness guarantee against withholding, automatic retention, key/password rotation or multi-peer CLI listener orchestration.

Windows tests cover a real trusted Syncthing daemon uploading to this receiver, then a fresh trusted Syrnck client restoring after the source connection stops and native GC runs. Other tests cover tiny/empty files, same-offset delta reuse, historical restoration, bounded maximum blocks, interrupted-block resume in plain/encrypted native Stores, simultaneous peer services with actual stream loss, wrong tokens/roles, queue ownership/limits and actual standalone process termination/restart. These checks do not establish power-loss durability, ARM64 support or Unreal-scale performance; Linux/macOS execution remains subject to current CI results.
