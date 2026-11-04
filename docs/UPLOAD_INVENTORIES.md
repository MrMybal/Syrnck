# Selective server upload inventories

Compatible clients and servers automatically use a paged inventory to retain
objects belonging to an unfinished upload. This works before the root manifest
arrives. Offline GC can reclaim unrelated objects while keeping registered
objects, including objects reused from an earlier version.

Older upload leases keep their conservative whole-Store hold. Existing command
lines still work; `--require-lease` requires upload leases, not this optional
inventory extension. A legacy hold anywhere in the Store still blocks GC.

## Registration and acknowledgement

The client validates its complete source closure, acquires an inventory-enabled
lease and registers each batch **before** querying remote presence for that
batch. Each page lists all objects in the batch, including reused ones. A page
contains 1..4096 strictly ordered unique object keys; ordering continues across
page boundaries. The client uses the existing deterministic closure ordering.

The server publishes an immutable page, then atomically replaces the lease's
committed page count and hash. It sends success only after both writes complete.
The existing file flush, atomic replacement and parent-directory flush rules
apply. There is one registration per batch, not a new journal write per object.

A lost response can safely retry the same page. A committed page must exist and
match its previous contents. It cannot be replaced through registration. A crash
after writing a page but before updating the lease leaves an unacknowledged page;
that page is outside the committed prefix. Retrying can validate and commit the
same staged page. The client must wait for registration acknowledgement before
relying on a presence response or sending objects for that page.

Each page includes a BLAKE3 checksum over its canonical descriptor and object
keys, linked to the previous page's hash. The lease commits the final hash along
with the page count. GC verifies the full committed chain, so a missing page,
altered key, broken link or altered page count stops payload deletion. This is
integrity checking under the Store's existing filesystem trust boundary; these
metadata are not authenticated against a malicious filesystem administrator.

## GC, expiry and cleanup

An active inventory lease retains the **existing objects explicitly listed in
its committed pages**. GC streams pages into the existing packed-object bitmap
and loose-object marks. Present objects pass the ordinary envelope, hash and
schema checks. Missing listed objects are allowed: registration can precede
arrival. GC does not materialize a second complete inventory of object IDs.

Registration is not proof that the root closure has arrived. Dependencies absent
from the registered inventory are not implicitly retained by that inventory.
Ordinary references, read leases and local transfer roots still provide their
own retention. Full closure and file-hash validation remain mandatory at final
reference publication. Ordinary object PUTs remain compatible and immutable;
unregistered uploads have no inventory retention guarantee.
`fsck` and `pack` keep their strict dependency audits and can reject incomplete
uploaded metadata until the transfer finishes or unretained metadata is collected.

Expiry and fencing remain as described in [upload leases](SERVER_UPLOAD_LEASES.md).
Before deleting payloads, GC durably marks observed expired leases and changes
their fences. Reactivation creates a fresh generation with zero pages and a new
initial hash. Resume registers its batches again before renegotiating presence.
Stale fences cannot register pages or publish references, including after a
lease ID has been pruned and recreated. Published receipts never retain payloads.

After stopping the server, `leases prune` removes inactive lease records and
unreferenced inventory generations. It validates lease metadata, committed active
chains and the complete inventory-directory shape before the first removal.
Active generations remain. Obsolete generation contents need not be usable
inventories, but their files must have the expected shape and cannot be symlinks.
Cleanup removes direct regular files and empty generation directories, with no
recursive traversal through unknown directories. The command's `removed` count
still counts lease records; it does not count inventory pages or directories.
Interruption during cleanup is safe to retry.

## Protocol and disk format

`GET /v1/capabilities` advertises `upload_inventories: 1` alongside
`upload_leases: 1`. New lease creation adds `inventory: true` to the existing
`PUT /v1/uploads/ID` body. That mode is immutable for the lease. Renewal preserves
the mode and current committed prefix; a newer server does not silently upgrade
an active legacy lease. A saved active inventory cannot silently downgrade when
the server stops advertising the extension.

`PUT /v1/uploads/ID/inventory/INDEX` accepts exactly:

```json
{"fence":"64-character lowercase hex token","objects":[{"kind":"chunk","id":"64-character lowercase hex ID"}]}
```

The request cap is **1 MiB** and successful registration returns **204**. Pages
must be contiguous from zero; identical retries below the current count are
accepted. Wrong/expired/released fences return 412, an unknown lease returns 404,
and malformed, conflicting, oversized or out-of-order pages are rejected.
Registration uses the normal bounded idempotent-request retry policy. Final
reference CAS is still never automatically retried.

Inventory-enabled lease records use schema **2**: existing fields plus
`inventory_pages` and `inventory_hash`. Legacy schema-1 leases remain readable.
The lease-record cap stays **16 KiB**, with at most **4096** records per Store.
Each inventory permits at most **4096 pages**, or **16,777,216 declared objects**.

Pages live in `STORE/upload-inventories/GENERATION/00000000.json`, where generation
is BLAKE3 of the JSON tuple `[lease_id, fence]`. Each page is at most 1 MiB and has
exactly `version`, `lease`, `fence`, `index`, `previous`, `objects`, and `checksum`.
The canonical checksum payload uses that field order without `checksum`, with
object keys encoded as `kind`, then `id`. The initial `previous` hash is BLAKE3 of
the JSON tuple `["syrnck-upload-inventory-v1", lease_id, fence]`; later pages use
the preceding checksum. The lease's final hash is the last checksum, or the
initial hash for zero pages.

These are ordinary local metadata: logical IDs and lease identities are visible,
without tokens, URLs or encryption keys. Payload encryption is unchanged. C++
applications must rebuild against the updated headers; object formats, Git
pointers, pack versions, local session schema and the C ABI are unchanged.
Only inventory-aware binaries may run maintenance on these Stores. GC and
cleanup remain offline, exclusively locked, and local to one server Store.
Distributed GC, object/user quotas and cross-server replication remain future work.

## Validation

`upload_inventories` exercises plain/encrypted and loose/packed retention before
manifest arrival, replay/conflict/ordering limits, reopen, corruption, page-count
integrity, cancellation, expired generations, clock rollback, missing committed
pages, overlapping retention roots, staged-page recovery, and pruning. `server_upload_leases` kills a production
agent after the server commits an inventory but before its reply, restarts the
server, collects unrelated data and resumes without object retransmission. It
also exercises capability downgrade rejection and legacy lease preservation.
Local validation runs on Windows x64; other platforms and sanitizer runs await CI.
Staged-page recovery models the two-write gap; it is not a power-loss validation.
