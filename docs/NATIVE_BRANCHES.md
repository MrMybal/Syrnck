# Native branches and recoverable switching

The native CLI has a symbolic current branch. Existing Stores without a HEAD
descriptor implicitly select `main`, including when that branch is unborn.
Reading HEAD never creates it. A successful switch records an explicit selection
bound to the canonical workspace directory.

```text
syrnck native head
syrnck native branch feature REVISION
syrnck native switch-plan feature --verify
syrnck native switch feature --progress
syrnck native status --combined
syrnck native add Source Content/Maps
syrnck native commit --staged -m "Selected feature changes"
syrnck native log
syrnck native switch main
```

`native branch NAME REVISION` creates a branch without selecting it or changing
working files. `native switch NAME` restores that branch's exact head and selects
it. **Neither branch head moves during switch**. `native checkout REVISION`
continues to restore an exact revision and move the selected branch, with an
archive of its previous head. Both use the [recoverable checkout](NATIVE_CHECKOUT.md)
executor. There is no detached HEAD or automatic branch creation during switch.

## Branch inventory and verified creation

```text
syrnck native branches --limit 100 --verify
syrnck native branches --after feature/topic --limit 100
syrnck native branch feature/topic REVISION --progress
```

`native branches` lists existing `heads/` references in byte-sorted name order.
It returns the current `head` descriptor, a `branches` array, `has_more`, an
exclusive `next_after` name cursor and `objects_verified`. An unborn selected
branch appears in `head`, without an invented entry in the array. Empty pages
have a null cursor. `--limit` is 1..1000, default 100; a cursor need not still
exist. Pages reflect the references present during each call, rather than a
snapshot spanning several invocations.

Each row includes `name`, `revision`, `current`, file/parent counts, the revision
timestamp and a UTF-8-safe message preview of at most 256 bytes, with
`message_bytes`/`message_truncated`. `index`, `index_base` and `stale_index` expose
the branch's frozen preparation, including stale indexes, without reading working
files. Orphan indexes without a branch remain accessible through `refs` and
`native staged --branch NAME`; the inventory does not adopt them as branch heads.

Metadata-only inventory does not audit block payloads. `--verify` audits the full
closures of the displayed heads and indexes, including their baseline histories.
The selected head outside that page is not an additional verified root. Reports
are bounded to 16 MiB. The implementation reads the reference table once for the
page, then resolves its rows without copying that table per branch; the table
itself is still loaded in full.

Creation accepts an existing exact revision in C++; the CLI also accepts normal
revision selectors. It verifies the complete retained data and publishes only if
both the destination head and staging slot are absent. Existing heads and orphan
indexes are errors. A pending checkout/switch blocks creation. There is no force
replacement. Creation neither selects the branch nor checks out its files; if
HEAD already symbolically names that unborn branch, its revision resolves to the
new head afterwards. Dirty working files remain unchanged.

Branch creation stays quiet on stdout in the CLI; its C++ result reports the
created name/revision. Inventory emits English JSON in either interface language.
Both commands accept the ordinary progress/cancellation options. Reference
publication uses the exclusive Store lock and expected absent slots. Cancellation,
missing retained bytes or observer failure before publication preserves existing
references and working files. No callbacks run after the commit point.

## CLI defaults and workspace identity

When omitted, `--branch` resolves the current branch for native commit (ordinary
or staged), add, staged, reset, status (ordinary or combined), checkout-plan,
checkout, log and history detachment. Explicit `--branch` keeps its existing
meaning: operate on that named branch, without changing HEAD. Explicit
`status --revision` selects the requested snapshot.

Snapshot arguments also accept [native tags and revision selectors](NATIVE_TAGS.md).
Branch switching still takes a literal branch name; a tag is restored with
`native checkout tags/NAME`, which moves the selected branch and archives its
previous head. It does not create detached HEAD state.

`native head` reports English JSON fields in both interface languages:

| Field | Meaning |
| --- | --- |
| `version` | Report schema version, currently 1 |
| `branch` | Current symbolic branch name |
| `revision` | Its current revision ID, or null for an unborn branch |
| `explicit` | Whether an explicit descriptor exists |
| `directory` | Bound canonical workspace, or null for legacy implicit HEAD |

HEAD resolves the branch dynamically. A later commit or checkout on that branch
changes `revision` without rewriting the descriptor. Generic deletion of its
branch leaves an unborn symbolic selection; it does not select another branch.

Once HEAD is explicit, CLI workspace commands require `--directory` (default
`.`) to resolve to that same canonical root. A Store supports one selected
workspace. Independent linked workspaces, relocation, detached revisions and
unborn destination switching remain future work. Low-level C++ methods with an
explicit branch argument keep their existing contract; they do not implicitly
resolve HEAD. Applications can use `native_head()` to select a branch themselves.

## Preflight, publication and recovery

`native switch-plan NAME [--verify]` is read-only. It starts with ordinary
checkout preflight against the current branch, targeting the destination head.
It adds `operation: "switch"`, `destination_branch`, `head_object` (the current
descriptor ID or null) and `destination_staging_snapshot`. An index on either
branch blocks switch, even if it has no staged changes; a destination index adds
the `target_staging_present` conflict. A completed blocked plan emits JSON and
exits 1, while invalid arguments or missing required objects emit no report.

Switch requires the destination to exist and point to a revision. Every tracked
source file must be clean by complete hash. Untracked/ignored collisions, links,
portable aliases and file/directory transitions follow the existing checkout
policy. There is no force, overwrite or automatic index reset option. A clean
switch to the already selected branch is a read-only no-op. Different branches
at the same revision still publish a new symbolic selection.

The transaction checks the expected source head, destination head, both staging
slots and previous HEAD before workspace application. Preparation verifies
complete closures and reconstructs targets. Exclusive file moves preserve
originals. The final atomic reference transaction publishes HEAD together with
the committed journal, source archive, target retention and receipt; both branch
heads are guarded without being moved. Before that boundary, HEAD remains the
original selection, even if working files have partly changed.

Inspect and recover with the existing commands:

```text
syrnck native checkout-status
syrnck native checkout-recover --progress
syrnck native checkout-recover --abort --progress
syrnck native checkout-status --id TRANSACTION_ID
```

Before publication, abort restores originals and keeps the old HEAD. Resume
finishes the new selection. After publication, recovery can only finish cleanup.
If a branch, staging slot or HEAD was changed through generic reference/admin
operations, recovery refuses rather than replacing that state. Store opening
does not automatically change working files. File moves are sequential and
external editors must be quiesced. Prepared targets and original backups remain
plaintext, including with encrypted object storage; all existing checkout
resource, cancellation, retention and private-data limits apply.

## Representation and C++ API

`workspace/HEAD` retains an ordinary chunk containing canonical JSON with
`version: 1`, `type: "native-head"`, `directory` and `branch`. It benefits from
existing compression/encryption/packing, with no new object kind or physical
format. The descriptor contains no frozen revision pointer. Branch references
continue to retain their own complete closures during GC.

Switch uses checkout journal version 2, adding `destination_branch`,
`previous_head` (nullable chunk ID) and `next_head` (chunk ID) to its hashed
identity. Version 1 checkout journals and receipts remain readable. GC is blocked
while either version is active. Older descriptor objects can be collected after
later switches; completed receipts remain inspectable because they do not need
those old descriptors to resolve their recorded fields. Active journals and HEAD
are local workspace state and must not be edited or replicated as ordinary
release roots. Older binaries do not interpret the new selection; use updated
binaries for native workspace commands after enabling it.

```cpp
auto head = store.native_head();
auto branches = store.native_branches({}, 100, true, operation);
auto created = store.create_native_branch("feature", revision_id, operation);
auto plan = store.plan_switch(project, "feature", true, operation);
auto switched = store.switch_directory(project, "feature", operation);
```

The C ABI and Go binding do not expose native branch operations yet. Regression
coverage includes legacy/unborn HEAD, equal-revision switches, default CLI
commands, staging on both branches, source edits, cancellation and reopen,
interrupted rollback, changed destination/HEAD, invalid descriptors and receipts
after GC. Plain/encrypted and loose/single-pack/multi-pack variants run locally
on Windows x64. Process-kill cases cover descriptor/journal writes, file moves,
HEAD publication and rollback. Unix behavior awaits the existing CI matrix.
