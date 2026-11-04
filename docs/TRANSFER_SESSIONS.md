# Persistent transfer sessions

Each push or pull records local recovery state before transferring objects.
The agent can resume in another process, retain verified blocks across a local
GC attempt and reconcile an acknowledgement lost at publication.

```text
syrnck agent sessions
syrnck agent resume https://blocks.example.invalid SESSION_ID --requests 4 --progress
syrnck agent forget SESSION_ID
```

`sessions` and `forget` are local commands and need no server token. Resume uses
the current `SYRNCK_TOKEN`, optional `--ca` and current transfer options. Encrypted
stores still require their local `SYRNCK_KEY`. Opening a Store starts no network
activity or background recovery.

Repeating the same push/pull automatically finds an unfinished session with the
same direction, endpoint, exact root and destination reference. Explicit resume
reads the root from the journal, so the original source reference can have been
removed. If a remote reference changed roots, a new `agent pull URL REF LOCAL`
addresses that new root; use the session ID to resume the original root.

## Format and writes

Records live in `STORE/transfers/ID.json`, with a strict version-2 schema and a
16 KiB limit. Version-1 records remain readable:

| Field | Meaning |
| --- | --- |
| `version` | Journal schema version, currently 2 |
| `id` | Stable BLAKE3 identity over direction, endpoint fingerprint, root and reference |
| `direction` | `push` or `pull` |
| `endpoint` | Hash of the client's canonical base URL |
| `root` | Exact logical object key requested |
| `reference` | Destination reference name |
| `expected` | Destination reference at session creation, or null |
| `state` | Recovery state described below |
| `upload_lease` | Nullable server upload lease ID (added in version 2) |

URL, token, encryption key, CA filename and exception text are not recorded.
Endpoint matching is exact after URL normalization; equivalent URL spellings
are not guaranteed to share a fingerprint. A different endpoint is rejected
before HTTP requests. These are ordinary local metadata, like `refs.json`,
rather than encrypted/authenticated journal contents. Filesystem access to the
Store remains the trust boundary.

Writes use the existing flushed-file and atomic-replacement machinery. On Unix,
creating the journal directory also flushes its parent; writes/removals flush
the journal directory. Windows uses the existing atomic publication behavior.
Uncommitted `.tmp...` stages do not change canonical state. The Store's exclusive
process lock coordinates journal/object/reference access. Unknown versions,
malformed records, mismatched identities and symlinked paths fail closed.
Power-loss validation remains separate from process-interruption testing.

Only operation boundaries write the journal. There is no per-block bitmap or
per-block flush: verified CAS objects identify completed payloads. Resume
renegotiates missing remote objects or checks local objects. Incomplete object
requests restart; complete objects are reused. Retry counts, pacing credit and
in-flight buffers start fresh in the new process.

## States and reference protection

| State | Resume behavior |
| --- | --- |
| `transferring` | Continue while the destination still matches `expected` |
| `publishing` | Publication was about to start, or may have completed before a crash |
| `uncertain` | Remote publication produced no unambiguous result |
| `conflict` | Destination changed; preserve it and require deliberate resolution |
| `complete` | Historical receipt; no GC hold or publication on explicit resume |

Before final publication the client durably records `publishing`. A remote
`publishing`/`uncertain` session reads references: a matching destination root
records completion without another reference PUT. Otherwise it keeps the hold
and reports `PublicationUncertain` (CLI exit **3**), even if the destination equals
the old `expected`. The server could have committed and then been deliberately
changed back. The original CAS is never blindly replayed.

A local pull validates the complete closure and publishes with compare-and-set
against saved `expected`. A changed destination is preserved. After a crash, an
already matching local root is verified and marked complete without downloading.
If the local reference still equals `expected`, publication can be retried. A
known conflict remains blocked until resolved/discarded; an already matching
desired root can also satisfy the session.

Normal success adds `session` and `reconciled` to the existing JSON statistics.
`reconciled: true` means recovery confirmed publication or returned a previously
completed receipt; object counters are zero because that invocation transferred
no objects. Resuming a completed receipt does not assert that references still
have their historical values or recreate them. A new push/pull after completion
reuses the ID with a fresh expected-reference snapshot and ordinary transfer.

A completion-journal write can fail after successful publication. Push reports
exit 3; pull reports an ordinary error. The durable `publishing` record allows
resume to inspect the reference. After remote CAS dispatch, cancellation and
callbacks remain disabled through completion recording, so a late stop can
still return success. The last cancellation boundary is the atomic publishing
marker: after it commits, the client finishes dispatch and receipt as well. A
crash in the marker/dispatch gap is deliberately treated as uncertain.

## GC and deliberate discard

Every unfinished local session blocks **GC preview and GC deletion** for the
entire local Store. A pull may contain metadata with missing dependencies; this
first implementation avoids trusting a partial retained closure. GC streams
journals with cancellation checks, without collecting all records in memory.
Invalid journals also prevent GC. Packing preserves logical objects, but its
full audit can reject incomplete metadata until the pull finishes or its
unreferenced objects are collected after deliberate discard.

`agent forget ID` removes the journal and releases its hold. It does not alter
references or remove payloads. Ordinary GC can then reclaim unreferenced objects,
including incomplete transfer metadata. A valid filename can be discarded even
if its contents are malformed. Completed receipts remain until forgotten and
do not pin their roots.

Local holds require journal-aware engines for every GC run. Compatible servers
also provide [expiring upload leases and publication receipts](SERVER_UPLOAD_LEASES.md).
Those receipts can confirm a lost acknowledgement even after a later reference
change. An expired lease can permit offline server GC; resume renegotiates and
reuploads missing objects from the protected source. Remote read leases for
pulls, selective/distributed retention, local journal pruning and background
recovery remain future work. Session listing materializes its result; receipts
consume disk space until removed.

## Validation

`transfer_sessions` checks schema/identity validation, plain/encrypted reopen,
cancellation before replacement, malformed records, path isolation, GC holds and
discard of incomplete metadata. Symlink cases run where creation is permitted.

`persistent_transfers` kills production agents at controlled HTTP gates during
push/pull and after remote commit but before acknowledgement. Fresh processes
resume, preserve concurrent local changes, reconstruct byte-identical encrypted
downloads and avoid replaying an ambiguous CAS. Tests also cover discard,
endpoint binding and token-free local commands. Local validation is Windows
x64; other platforms await CI.
