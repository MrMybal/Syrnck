# Repository-local asset leases

`syrnck::AssetLocks` provides an atomic lease table in an existing Store. The C++
API and bilingual CLI support grouped acquisition, renewal, release and expiry
pruning. [Server access](SERVER_ACCESS.md) connects this table to authenticated
HTTP identities, remote lock operations and branch-publication enforcement.
The local C++ caller supplies a trusted owner identifier. A fence is a
stale-operation guard, not an authentication credential.

## Acquire and inspect

```text
syrnck locks acquire-plan --owner alice --seconds 3600 Content/Map.uasset Content/Map.ubulk
syrnck locks acquire --owner alice --seconds 3600 Content/Map.uasset Content/Map.ubulk
syrnck locks list --limit 100
syrnck --lang fr locks list
```

All paths in one acquisition publish together, or none do. This supports related
Unreal asset files without partially reserving a group. Locks name exact
repository-relative paths; they do not lock directory subtrees or scan working
files. Unicode normalization and case folding identify the same portable path.
Duplicate portable aliases in a request are rejected. Every active lock blocks
another acquisition, including one requested by its existing owner.

An acquisition plan reads metadata only and stores no objects. A ready plan
exits 0; a blocked plan exits 1 with JSON conflicts. The plan does not reserve
paths or guarantee a future generation. Acquisition recomputes current state
under the Store lock. Other invalid commands emit a diagnostic without a partial
success result. JSON fields and reason codes remain English in both languages.

`locks list` includes owner, original path spelling, fence, acquisition/expiry
timestamps and an `active` flag. Expired records remain visible until replaced,
released or pruned. Pages default to 100 entries, with a limit in 1..1000 and an
optional `--after PATH` using the previous page's `next` value. Ordering uses
portable folded paths.

## Renew, release and prune

```text
syrnck locks renew --owner alice --fence 1 --seconds 3600 Content/Map.uasset Content/Map.ubulk
syrnck locks release --owner alice --fence 1 Content/Map.uasset Content/Map.ubulk
syrnck locks prune
syrnck locks prune --delete
```

Use the fence returned by acquisition. Renew/release require the owner and every
claimed fence to match. A single mismatch rejects the complete batch. CLI batches
use one shared fence; C++ claims can carry a different fence for each path.

Renewal preserves the acquisition time and fence, extends expiry without
shortening it, and requires the entire group to remain active through publication.
Expired locks require a new acquisition. Release accepts an expired matching
claim, but a reallocated path rejects the former owner's claim. Pruning previews
expired records by default; `--delete` removes the selected expired records in
one expected-reference transaction. Active records remain protected.

Each acquisition advances a Store-wide unsigned 64-bit fencing generation. A
retained counter survives release, expiry pruning and GC, so normal API operations
never recycle an old generation. Exhaustion refuses new acquisition. Expired paths
can be acquired directly without a preceding prune; replacement receives the new
generation. Manual changes to the internal roots bypass this component's contract.

Durations are in 1..86400 seconds. Production uses Unix wall-clock seconds; the
service calling the API must supply a trustworthy clock. Publication rechecks
expiry and refuses an observed backwards clock movement. A supplied C++ clock is
an application dependency, not a client-controlled lease parameter; the CLI has
no clock override.

## Storage and API

The table uses ordinary immutable chunk objects and one atomic reference update:

| Reference | Purpose |
| --- | --- |
| `asset-locks/clock` | Retained fencing generation |
| `asset-locks/paths/BLAKE3_FOLDED_PATH` | Current immutable lease record |

The record contains `version`, `type`, `path`, `owner`, `fence`, `created_at` and
`expires_at`. Readers validate its schema, canonical bytes, path identity,
timestamps and relationship to the fencing counter. Existing object encryption
protects stored records at rest. Packing and GC retain current table roots;
obsolete unreferenced records can be collected. Lock records do not retain file
contents: branches, revisions, tags and pins retain the asset versions themselves.

The table holds at most 4096 records, including expired entries. Requests contain
1..1024 paths; each path is at most 4096 UTF-8 bytes. Owner identifiers contain
1..256 UTF-8 bytes without control characters. Existing repository path rules
apply. Applications must preserve the Store's exclusive lock and avoid callback
reentry. Cancellation before reference publication preserves the visible table
and can leave complete unreferenced metadata objects.

```cpp
#include <syrnck/asset_locks.hpp>

syrnck::AssetLocks locks(store);
auto acquired = locks.acquire("alice", {"Content/Map.uasset", "Content/Map.ubulk"});
const auto generation = acquired["locks"][0]["fence"].get<std::uint64_t>();
locks.renew("alice", {{"Content/Map.uasset", generation},
                       {"Content/Map.ubulk", generation}}, 3600);
```

C++ tests exercise grouped lifecycle, ownership/fence mismatches, exact expiry,
publication-time expiry/clock changes, strict metadata validation, cancellation,
pagination, encrypted/packed storage and generation retention after GC. CLI tests
exercise Unicode paths/owners, bilingual help/errors and grouped operations.
