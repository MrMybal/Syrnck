---
id: syncthing
title: Syncthing fixed-block adapter
description: Syncthing fixed-block adapter guide.
section: API
order: 660
parent: integrations
revision: 3
---

# Syncthing fixed-block adapter {#syncthing-fixed-block-adapter}

This optional Go module connects the C++ engine to Syncthing's BEP block model.
It depends on the unmodified Syncthing library at **v2.1.7**, commit
`d6d0181f7d46`, represented by the Go pseudo-version pinned in `go.mod`/`go.sum`.
The core Go binding remains a separate module without external Go dependencies.
This module requires Go 1.26.2 or later; local validation used Go 1.27.3.

The adapter supports regular file data and explicitly paired, authenticated
device connections. It is not a resident synchronization daemon, a drop-in
Syncthing executable or an implementation of its database, discovery, pairing UI,
durable version-vector reconciliation, workspace deletions/renames or conflict
policy. The embedding application still owns those folder decisions.

## Native library and dependencies {#build-and-test}

Build `syrnck_ffi` and prepare the pinned Go dependencies once with `go mod download` from `bindings/syncthing`. The adapter is optional and separate from the core binding. Linux/macOS use cgo and their loader path; those platforms and ARM64 remain to be validated. Distribute the dependencies and notices required by the chosen integration.

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

The application supplies certificate storage/rotation, the trusted Device ID,
scheduling, persistent folder/index state, version vectors, tombstones and
conflict handling. Transport encryption authenticates and protects this peer
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
and refuses adapter/reference version mismatches or modified manifests.

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
it does not establish a durable, autonomous folder service. Without the explicit
binary, the real-daemon test reports a skip; ordinary adapter tests still run.

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
The current synchronous ingest/verification ABI cannot be cancelled mid-call.
A late cancellation or a detected change during ingestion can therefore leave
a complete internally retained manifest; its ID is returned alongside the
error. Callers must handle the error before publishing that result. No partial
working file is installed. A process kill can leave private temporary data;
power-loss durability and automatic staging reclamation are not established.
