# Selective transfer retention and receipt pruning

Offline GC now treats unfinished local transfers and active server read leases
as retention roots. Objects unrelated to those roots can be reclaimed while a
transfer is interrupted. The server must still be stopped before another process
opens its Store.

| Protection source | GC behavior |
| --- | --- |
| Ordinary references and pins | Verify and retain the complete closure and file hashes |
| Unfinished local push | Verify and retain its complete source closure |
| Unfinished local pull | Retain every available object reachable through its saved root |
| Active server read lease | Verify and retain the complete leased closure |
| Active inventory-enabled server upload lease | Retain existing objects in committed inventory pages |
| Active legacy server upload lease | Block all GC until expiry, publication or deliberate release |
| Completed local receipt or inactive server lease | Does not retain payloads |
| Invalid journal or lease metadata | Fail before payload deletion |

## Incomplete pulls

A pull imports verified metadata before scheduling its dependencies. GC follows
that metadata and retains every dependency already present, including blocks
after gaps in a manifest. Missing objects are expected for an interrupted pull;
they do not enter the live-object marks. A missing root therefore retains no
payload by itself. Objects whose association cannot be discovered through the
available metadata can be collected and will need downloading again.

Existing corrupt envelopes, invalid metadata or a wrong length for an available
file block stop collection. A complete manifest also has its whole-file hash
verified. An incomplete manifest cannot yet have its whole-file hash checked;
ordinary final publication still requires full verification. `fsck` and `pack`
remain strict and can reject incomplete orphan metadata while a pull is pending.

GC first verifies complete roots, then adds available partial closures to the
same packed-object bitmap and loose-object marks. It does not materialize a
second complete object inventory. Transfer roots, pending traversal and parsed
metadata still consume memory; these allocations are not a fixed RAM cap.
Cancellation and the existing catalog/deletion commit boundary remain in force.

A read lease contributes its exact root at the start of the collection. If it
expires while GC is running, this invocation conservatively keeps its objects.
Previously observed expired generations are still durably fenced before deleting
payloads, preventing clock rollback from reviving a collected generation.

New [server upload inventories](UPLOAD_INVENTORIES.md) register batches before
remote presence queries, retaining declared objects even before manifest arrival.
Legacy upload leases keep their global hold. Multi-server/distributed retention
is still future work; local process locks and leases do not implement distributed consensus.

## Pruning completed local receipts

```text
syrnck agent prune
syrnck agent prune --delete --progress
```

The default is an inspection only. JSON reports `completed`, `unfinished`,
`removed` and `deleted`. `--delete` removes **all complete receipts**, regardless
of age. There is no timestamp or age policy in the current journal schema.
Progress reports metadata bytes scanned and removed; JSON result counts are records.

Both passes stream the journal directory without keeping every ID in memory.
The first pass validates the entire inventory before any removal. The second
pass removes only records whose state is `complete`. `transferring`, `publishing`,
`uncertain` and `conflict` records remain intact, along with their retention roots.
Malformed records stop pruning; use explicit `agent forget ID` if deliberately
discarding a damaged or unfinished journal.

Pruning is local, requires no server token and never contacts a server. Opening
an encrypted Store still requires its key. References, objects and remote leases
are untouched. A removed receipt can no longer be resumed by its session ID;
a new push/pull creates ordinary transfer state instead of returning that receipt.

Cancellation or an IO/progress error during deletion can leave a prefix of
completed receipts removed. Retrying is safe. Successful removal flushes the
journal directory; an interrupted pass also attempts that flush. This is not an
atomic multi-record transaction. Neither `agent prune` nor `agent forget` releases
a remote lease; abandoned leases expire or require explicit administration.

## Validation

Loose-object inventories also handle an observed Windows filesystem race: an
unknown uppercase-hash `.tmp` entry can briefly appear while deleted data is
being cleaned up. As with lease/journal inventories, an unknown `.tmp` path is
ignored only after its disappearance is observed within a bounded 50 ms wait.
Canonical object names never take this exception. Persistent unknown files,
symlinks and malformed object names still fail the inventory before GC deletion.
The creating process was not identified; this is a bounded filesystem tolerance,
not a general rule to accept extra Store files.

`transfer_retention` covers partial files with gaps, mixed packed/loose objects,
plain/encrypted Stores, GC cancellation, reopening and byte-exact reconstruction,
missing versus corrupt data, complete-file hash failures, full source/read roots,
and two-pass pruning with malformed journals and cancellation after a removal.
It also checks refusal of a persistent foreign temporary entry without deleting
live/unrelated payloads, and successful GC after the temporary entry disappears.
Production process-kill transfer tests resume after selective local/server GC.
CLI tests exercise token-free pruning with progress and unfinished records.
Validation runs on Windows x64; other platforms and sanitizer runs await CI.
