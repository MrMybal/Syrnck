# Syncthing fixed-block adapter

This optional Go module connects the C++ engine to Syncthing's BEP block model.
It depends on the unmodified Syncthing library at **v2.1.7**, commit
`d6d0181f7d46`, represented by the Go pseudo-version pinned in `go.mod`/`go.sum`.
The core Go binding remains a separate module without external Go dependencies.
This module requires Go 1.26.2 or later; local validation used Go 1.27.3.

The adapter supports regular file data. It is not a resident synchronization
daemon, a drop-in Syncthing executable or an implementation of its database,
discovery, device pairing, version-vector reconciliation, deletions, renames or
conflict-copy policy. The embedding application owns those decisions.

## Build and test

Build `syrnck_ffi` first. Download the pinned public Go dependencies once from
this directory with `go mod download`. Then, from the repository root:

```text
python scripts/test_go.py --library build/Release/syrnck_ffi.dll --module syncthing --check-pointers
python scripts/test_go.py --library build/Release/syrnck_ffi.dll --module syncthing --benchmark
```

The runner uses the project's Go caches, `-mod=readonly` and `GOPROXY=off` for
adapter tests. If preparing dependencies manually, use the same `GOPATH`
(`.tools/go-path`) or populate that cache before running the script. To add
the tests to CTest, configure with `-DSYRNCK_TEST_SYNCTHING=ON`. The option is
off by default so ordinary core builds do not require the Syncthing dependency.
Linux/macOS use the existing cgo backend and their normal shared-library path;
those adapter executions and ARM64 still require validation.

## APIs

`NewFile(ctx, store, manifest, folder, name, blockSize)` creates an immutable
servable view. It verifies the engine's complete file hash without exporting a
file, then reads verified ranges and constructs Syncthing `protocol.FileInfo`
SHA-256 descriptors. Engine CDC/fixed chunks and BLAKE3 object IDs stay intact.
A zero block size chooses Syncthing's own size policy; explicit sizes must be
powers of two from 128 KiB through 16 MiB. An index is capped at 65536 blocks.
Empty files use the reference implementation's single empty SHA-256 block.

`File.Info()` returns an independent index copy. Set application-owned version
vectors, sequence numbers and timestamps on that copy before announcing it.
Names must be valid NFC UTF-8 relative paths. Native Windows separators from
Syncthing's model callbacks are converted to the protocol's `/` separators.

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

## Resume and cancellation

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

## Interoperability evidence

Tests compare descriptors with the actual Syncthing scanner, including empty
files, and transfer indexes/requests/responses through the actual Syncthing
`protocol.Connection` implementation. The wire tests exchange Hello and cluster
configuration over a loopback TLS 1.3 connection with verified client/server
certificates, then exercise raw and compressed BEP messages. Encrypted engine
Stores reconstruct the exact file while only its changed 128 KiB block travels.

Other tests cover moved/repeated reuse across different engine chunk layouts,
corrupt peer data, invalid indexes, cancelled transfers, staged-byte corruption,
changed-index refusal, hard process termination/resumption and independent index
copies. This validates the regular file data subset against the pinned reference library. A full Syncthing daemon
pairing/folder synchronization test remains outstanding.

The initial synthetic Windows x64 measurements on a Ryzen 9 7900X were roughly
184–204 MB/s for direct verified 128 KiB native range reads and 170–201 MB/s with
the additional BEP SHA-256 check. A 512 KiB differential reconstruction transferred
128 KiB, reused 384 KiB and took about 27–34 ms. These are short warm-store runs on
highly compressible fixtures, not an Unreal corpus, isolated ABI overhead or
production throughput. The current implementation rebuilds SHA-256 indexes,
performs additional complete verification and stages whole files; persistent
indexes and streaming ingestion are future performance work.

Protocol semantics: [official BEP specification](https://docs.syncthing.net/specs/bep-v1.html).
Reference library: [Syncthing v2.1.7](https://github.com/syncthing/syncthing/tree/v2.1.7).
Syncthing is an external MPL-2.0 dependency, not vendored implementation code.
Its other module dependencies retain their respective licenses.
