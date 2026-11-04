# Server read leases

Pulls automatically negotiate a read lease before their first missing object is
requested from a compatible server. It binds the exact root, independently of
its current reference name. An interrupted download can therefore resume after
that reference changes or is removed, provided its lease remains active.

```text
syrnck serve --read-lease-seconds 3600
syrnck agent pull https://blocks.example.invalid asset received --require-read-lease --requests 4
syrnck agent resume https://blocks.example.invalid SESSION_ID --require-read-lease
```

`--require-read-lease` rejects a server without this extension before downloading
any missing object. An entirely local closure needs no remote lease, even with
this option. The existing `--require-lease` option remains specific to uploads.
Object GETs remain available to legacy clients; this is client-negotiated
retention, not a server access-control policy.

## Lifecycle and retention

Before acknowledging a new or expired lease, the server verifies the complete
root closure and full file hashes. Missing or corrupt sources fail acquisition.
Live renewal avoids repeating this full scan. All Store operations must be
serialized by the embedding application; the supplied server uses its Store
mutex and exclusive process lock.

The TTL accepts **1..86400 seconds**, default **3600**. Expiry uses server UTC;
whole-second timestamps round up by at most one second. The client schedules
renewal using a monotonic clock at one third of the TTL. An independent cancellable
request renews without blocking the object scheduler. It never accesses the local
Store or invokes user progress observers. Control responses are capped at 16 KiB
and sit outside object-buffer and payload-throughput statistics.

Short TTLs, long local validation, user callbacks, server contention or an
unreachable server can still cause expiry. Acquisition or renewal after expiry
gets a new fence; a client observing a changed renewal fence aborts and requires
resume. Protection is not guaranteed across an expiry gap.

Active read leases retain their **complete verified root closures** during offline
GC. Unrelated objects remain collectable. The server can be killed and its
reference removed offline without releasing this hold. Before deleting objects,
GC durably marks observed expired leases `expired` and rotates their fence, so a
later wall-clock rollback cannot reactivate a generation whose objects were
collected. Dry runs do not write expiry transitions. Active upload leases can
independently block collection. See [selective retention](SELECTIVE_RETENTION.md).

After all object requests drain, the client cancels and joins any renewal and
releases the read lease **before** local reference publication. Source retention
is no longer needed: verified objects are local and the unfinished local session
retains those objects during local GC. Local compare-and-set then verifies the closure and file
hashes before publishing. Cancellation, corruption or a changed local destination
cannot silently publish an incomplete version.

Release is fenced and idempotent. If its reply is lost, the ordinary bounded
retry policy applies. A release error leaves the local session unfinished;
resume retries cleanup using the saved root and lease. A missing/pruned lease
counts as released once no remote data is needed. A completed local receipt
resumes without contacting the server or recreating historical references.
An abandoned transfer otherwise retains its source until expiry or an explicit
administrative release. `agent forget` only removes the local journal.

## Protocol

All routes use the existing authenticated HTTP/HTTPS transport and server token.
`GET /v1/capabilities` advertises `read_leases: 1` and `read_lease_seconds`.

| Request | Result |
| --- | --- |
| `GET /v1/reads/ID` | Lease and TTL; 404 if absent |
| `PUT /v1/reads/ID` | Acquire/renew with exactly `{root, fence}`; null fence for creation |
| `POST /v1/reads/ID/release` | Release with exactly `{fence}` |

Successful responses contain exactly `{lease, lease_seconds}`. IDs and fences
are independent random 64-character hexadecimal strings. The strict lease schema
has `version: 1`, `id`, `root`, `fence`, `expires_at` and `state`. State is `active`,
`expired` or `released`. Root identity cannot change. Wrong fences and attempts
to reactivate a released record return 412. Recreating a pruned ID generates a
fresh fence. This fences renewal/release, not immutable hash-addressed GETs.

Records live in `STORE/reads/ID.json`; at most **4096** records, including inactive
ones, are permitted. Symlinks, malformed metadata and unknown schemas fail
closed. Files use the existing flushed atomic write machinery.
Transient foreign `*.tmp` entries may disappear within a bounded 50 ms wait;
persistent unknown files still fail closed, and canonical lease names are never skipped.
Metadata contains no tokens, URLs or encryption keys; it has the same filesystem trust boundary
as references. Stores encrypted at rest still have ordinary lease metadata.

Local transfer journals now write schema **3**, adding nullable `read_lease` for
pulls; schemas 1 and 2 remain readable. Transfer statistics add `read_lease`
alongside `upload_lease`. An identifier is a historical association, not evidence
that its hold remains active.

A capability 404, explicit `read_leases: 0`, or an older valid upload-only
capability response permits legacy fallback unless protection is required.
Malformed responses and other failures propagate. A saved read lease cannot
silently downgrade to an unprotected download.

## Offline administration

Stop the server before opening its Store with the CLI. Encrypted stores require
their key; these local commands do not require the server token.

```text
syrnck leases list --read
syrnck leases release ID --read
syrnck leases prune --read
```

Prune validates the entire bounded inventory before removing inactive/expired
records. Deliberate release can interrupt an active reader's retention guarantee.
Older binaries do not understand these holds and must not run GC on this Store.
Leases are process-local server retention, not replication or
distributed consensus. Startup/renewal checks do not provide backup guarantees
against disk corruption, external file deletion or an administrator removing
the lease directory.

## Validation

`read_leases` exercises plain/encrypted reopen, expiry fencing, simulated clock
rollback, missing sources, schema validation, release, pruning and fail-closed GC.
`server_read_leases` kills production agents and servers, unpins the source,
resumes the saved root, verifies encrypted reconstruction, exercises async renewal
and cancellation, drops a release acknowledgement and checks legacy fallback.
These checks were run on Windows x64; Linux/macOS/ARM and sanitizer validation
remain for CI. Process interruption tests do not establish power-loss durability.
