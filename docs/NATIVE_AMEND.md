# Native commit amendment

Amendment replaces an existing branch tip while preserving its ordered parent
list, including every parent of a merge revision. The previous revision remains
immutable and is retained under `archive/amend/OLD_ID`. The branch move, archive
root and optional staging consumption publish in one atomic reference update.
Existing tags continue to identify their original revision.

## CLI

```text
# Replace the tip's snapshot with the current selected directory.
syrnck native commit --amend -m "Corrected project snapshot"

# Use exactly the prepared bytes, leaving later working edits alone.
syrnck native add Content/Maps Source
syrnck native commit --amend --staged -m "Corrected selected snapshot"

# Reuse every existing file entry and change the message.
syrnck native commit --amend --message-only -m "Corrected message"

# Guard scripts against amending a different tip from the one inspected.
syrnck native commit --amend --message-only --expect OLD_ID -m "Correction"
```

All modes require `-m`/`--message`. The selected native branch is the default;
`--branch NAME` overrides it without changing symbolic HEAD. `--expect` is an
exact lowercase 64-character revision ID, never a branch/tag selector. If omitted,
the CLI captures the selected branch's current revision while holding the
exclusive Store lock. There must already be a tip; amendment cannot create an
unborn branch. `--message-only` and `--expect` require `--amend`.

| Mode | Snapshot source | Staging behavior | Working files |
| --- | --- | --- | --- |
| `--amend` | Complete current directory, using ordinary commit exclusions and ingestion settings | Any existing index is rejected | Read and ingested, never written |
| `--amend --staged` | Immutable branch staging snapshot | Must exist and have the current tip as baseline; consumed atomically | Never accessed |
| `--amend --message-only` | Existing tip's file entries | Any existing index is rejected | Never accessed |

An unchanged prepared snapshot is accepted for a message correction. Staged and
message modes are mutually exclusive and reject directory/pipeline arguments.
Directory mode accepts `--directory`, `--workers` and `--buffer-mib` as ordinary
commit does, including workspace binding checks. All modes refuse to run while a
native checkout or switch requires recovery. A stale index requires inspection
and explicit reset; amendment never silently changes its baseline.

The new revision uses the supplied message and current UTC timestamp in seconds.
The v1 revision format has no author identity or signature fields. An amendment
whose complete serialized metadata is identical can return the existing ID;
there is no random nonce that forces a new object identity.

## Retention, interruption and restoration

Before publication, the engine verifies the complete replacement closure and
the archive closure, including reconstructed file hashes. A missing old block
prevents publication even if the new snapshot no longer needs that block.
Expected branch, index and archive values are checked together. An existing
`archive/amend/OLD_ID` can be reused only if it points to that same revision;
an unrelated object at that name is an error.

Cancellation, callback failure or invalid input before reference publication
keeps the old branch, index and archives. Preparation may leave immutable,
unreferenced objects that a retry can reuse or GC can collect. A process stopped
during publication leaves the complete old or complete new reference set.
Once publication starts, it may complete despite cancellation; there are no
callbacks or cancellation checks after a successful publication.

After an interrupted invocation, inspect `native head`, `refs` and `native
staged`. If the expected old tip remains, retry with its exact `--expect` ID. If
the replacement tip and `archive/amend/OLD_ID` exist, publication completed. When
the replacement has a different ID, retrying with the old expected ID fails
instead of amending that replacement a second time.

Archives have no automatic expiry or dedicated pruning command. They are GC
roots for the old revision, its ancestors and all referenced file blocks. Even
when ordinary history no longer visits an old tip, it remains exportable:

```text
syrnck native export OLD_ID recovered-version
syrnck native branch recovered OLD_ID
```

Use the existing checkout/switch preflight and recovery workflow to restore an
archived snapshot into a working directory. Generic `unpin` can remove an archive
root; removing the last retaining root makes its exclusive blocks eligible for
GC. Retention is a local storage guarantee, not an access-control boundary.

## C++ API and validation

```cpp
auto expected = store.resolve_native_revision("heads/main");
auto corrected = store.amend_directory(project, "main", expected, "Corrected snapshot");
auto prepared = store.amend_staged("main", corrected, "Selected correction", operation);
auto renamed = store.amend_message("main", prepared, "Corrected message", operation);
```

Every API requires the exact expected tip ID. The directory overload accepts
`IngestOptions`, defaulting to repository settings. Staged/message APIs accept
`OperationOptions`; callbacks must not reenter the Store. Metadata-only here
describes snapshot selection: publication still reads and verifies stored file
payloads. All CLI modes support English/French progress and cooperative Ctrl+C
cancellation; JSON progress and returned IDs are language-independent.
Native amendment is not yet exposed through the C ABI or Go binding.

Tests cover root and merge ancestry, immutable archives/tags, staged bytes versus
later working edits, clean indexes, stale baselines, wrong expected IDs, pending
checkout refusal, archive conflicts, missing old/prepared payloads, cancellation,
reopening and destructive GC. Plain/encrypted Stores run with loose objects,
one pack and multiple packs. Process-kill cases exercise chunk, manifest,
revision and atomic branch/archive/index publication boundaries, followed by
read-only inspection and retry using the production core.
