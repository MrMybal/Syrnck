# Persistent server upload leases

New clients automatically protect uploads when the server advertises this
extension. Active leases survive server restarts and block offline GC. A lease
expires when its client stops renewing it; published references then provide
ordinary retention after a successful transfer.

```text
syrnck --store server-data serve --lease-seconds 3600 --require-lease
syrnck agent push https://blocks.example.invalid files/MANIFEST asset --require-lease
```

The server still requires `SYRNCK_TOKEN`, a loopback bind and an HTTPS proxy for
remote access. Encrypted stores use their own `SYRNCK_KEY`. English/French CLI
text is supported; protocol fields remain English. This extension currently
covers **uploads**, not remote read leases for pulls.

## Lifetime, GC and administration

The default duration is **3,600 seconds**, configurable with `--lease-seconds`
in **1..86,400 seconds**. Whole-second expiry is rounded up by at most one second.
The client schedules renewal using its monotonic clock at one third of the
advertised duration, without requiring synchronized client/server wall clocks.
The server uses its wall clock for persisted expiry, so operators must maintain
that clock. A backward jump can extend retention; a forward jump can expire it.
Before GC deletes payloads, it durably records observed expirations and replaces
their fences. A later clock rollback cannot revive a fence whose negotiated
objects might have been collected. Expired publication rejection also records
that observation. GC preview does not write these transitions.

The coordinator schedules renewals during object retry and traffic-pacing waits.
At most one renewal worker uses a separate HTTP handle, so a delayed control
reply does not stop object IO. It never accesses the Store or invokes user
progress callbacks; the coordinator consumes its result and validates the fence.
The worker is cancelled/joined on exit and drained before publication. Its small
control buffers/connection are outside the object request/buffer counters.
Handles are independent, following libcurl's [thread-safety requirements](https://curl.se/libcurl/c/threadsafe.html).

Local IO, user callbacks and synchronous control requests already running can
delay scheduling. A short TTL cannot guarantee continuous protection through an
arbitrarily long stall. Keep the normal TTL large enough for those operations;
one-second leases are useful for controlled tests rather than deployment guidance.

An active, unexpired lease blocks **GC preview and deletion for the whole server
Store**. GC streams and validates the records before planning. An expired,
released or published lease does not retain objects. Expiry never deletes
payloads itself; an ordinary GC is still required. The server holds the Store's
exclusive process lock while running, so current maintenance is offline.

Stop the server before local administration:

```text
syrnck --store server-data leases list
syrnck --store server-data leases release LEASE_ID
syrnck --store server-data leases prune
```

`release` deliberately revokes an active lease. It does not modify references
or payloads and leaves published receipts intact. `prune` removes expired,
released and published records after validating the full inventory. Live leases
are preserved. Removing a published receipt can reduce what a disconnected
client can prove, but never authorizes replaying its old publication.

The store permits at most **4,096 lease records**, including receipts. New lease
creation fails at capacity until inactive records are pruned. Records are at
most 16 KiB; listing materializes at most that bounded inventory. Malformed
records and symlinked paths fail closed. Every GC binary must understand this
extension; older binaries ignore server upload holds.

## Fencing and resume

The client persists a random lease ID in its local journal before asking the
server to create it. The server binds it to the exact root, destination reference
and expected previous reference. Renewing a live lease keeps its fence. Expiry
reactivation or recreation after pruning produces a fresh random fence.

Every leased reference PUT carries that fence. Expired, released and stale
fences fail before reference publication. Thus a delayed request cannot publish
under a lease reactivated by a later attempt, including when an ID was pruned
and recreated. This is coordination within one exclusively locked Store, not a
distributed consensus or multi-server fencing guarantee.

If renewal changes the fence during an upload, the client stops and requires
resume to renegotiate missing objects. It cannot rely on a presence query made
before a retention gap. Resume may reupload objects reclaimed after expiry;
the unfinished **local** journal still protects the source against local GC.

Before publication the server validates the full closure and performs the saved
reference CAS. It then records a durable `published` receipt. These are two
atomic writes: a crash between them leaves an active lease and a possibly
published reference. Resume can confirm a matching root and write the receipt
without another reference PUT.

A published receipt is bound to the original descriptor. Repeating its leased
publication acknowledges that receipt and never rewrites a reference changed
later. A client recovering a lost acknowledgement can therefore finish from the
receipt even after another writer changed the reference. If no receipt exists
and the reference no longer matches, the existing uncertain-publication policy
applies. Receipts do not pin old versions after their references are removed.

Cancellation retains the upload lease until expiry or deliberate release. Local
`agent forget` remains a local command and does not contact the server. Lease
records contain operational IDs/fences and object keys, not bearer tokens or
encryption keys. The service still uses one shared authorization token and has
no per-user lease ownership or ACL model.

## Protocol and compatibility

All endpoints use the existing bearer authentication:

| Request | Behavior |
| --- | --- |
| `GET v1/capabilities` | Advertises `upload_leases: 1` and `lease_seconds` |
| `GET v1/uploads/ID` | Returns `lease` metadata and configured `lease_seconds`, or 404 |
| `PUT v1/uploads/ID` | Creates/renews an exact descriptor; body has `root`, `reference`, `expected`, `fence` |
| `POST v1/uploads/ID/confirm` | Confirms a matching reference without publishing it; body has `fence` |
| `PUT v1/refs/NAME` with `upload: {id, fence}` | Validates the lease and performs/reconciles its bound CAS |

Creation sends a null fence. A retried creation is idempotent for the same
descriptor; renewals send the last observed fence. Lease request and response
bodies are capped at 16 KiB in the client; the server rejects oversized control
requests before parsing. Server records in `STORE/uploads/ID.json` use a strict
version-1 schema: `version`, `id`, `reference`, `root`, `expected`, `fence`,
`expires_at` (Unix seconds), and `state` (`active`, `expired`, `published`, `released`).
Writes use the same flushed-file and atomic-publication mechanisms as journals.

HTTP 412 rejects an expired/released/fenced leased publication; 428 rejects an
unleased publication when the server requires leases. The client treats these
as known rejection only on its leased publication path. Ambiguous errors still
exit 3, and final reference PUTs are never automatically retried.

A 404 capability endpoint or explicit `upload_leases: 0` allows legacy upload
when protection was not required. Network errors, malformed capabilities and
loss of support for a saved lease fail rather than silently downgrade. Client
`--require-lease` rejects unsupported servers before object upload. Server
`--require-lease` rejects unleased final reference PUTs; legacy object PUTs remain
available. The flag is for push and resumed push, not pull.

Local transfer journals now write schema **2**, adding nullable `upload_lease`;
schema-1 journals remain readable. Final statistics add nullable `upload_lease`.
That ID identifies the negotiated lease, not its current liveness. Logical
objects, Git pointers and C ABI formats are unchanged.

`upload_leases` exercises plain/encrypted reopen, expiry, persisted expiry across
modeled clock rollback, stale fences, receipts,
release/prune and corrupt records. `server_upload_leases` uses the production
server and client for offline GC protection, expiry/reupload, renewal under a
rate limit, object IO during a delayed renewal, cancellation/join of its worker,
coordinator-only observers, authentication, strict/legacy behavior, and a real killed client
whose publication response is held by a proxy. It verifies reconciliation after
a later reference change without replay. Local validation is Windows x64;
other platforms and power-loss validation remain separate work.
