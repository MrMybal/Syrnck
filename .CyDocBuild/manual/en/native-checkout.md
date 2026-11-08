---
id: native-checkout
title: Recoverable native checkout
description: Recoverable native checkout guide.
section: Version control
order: 230
parent: native-history
revision: 2
---

# Recoverable native checkout {#recoverable-native-checkout}

`native checkout` restores an exact stored revision into an existing workspace
and moves the explicitly selected native branch to it. The previous branch head
is archived before any of its retention roots are released. This is the first,
restricted executor of the [checkout preflight](doc:native-workspace).

```text
syrnck native checkout-plan REVISION --branch main --verify
syrnck native checkout REVISION --branch main --directory . --progress
syrnck native checkout-status
syrnck native checkout-recover --progress
syrnck native checkout-recover --abort --progress
syrnck native checkout-status --id TRANSACTION_ID
```

The default branch is the [current native branch](doc:native-branches), implicitly
`main` in legacy Stores; the default directory is `.`. Selecting `--branch feature`
moves that branch, using its current snapshot as the workspace baseline.
`native switch BRANCH` selects a different branch while preserving both heads.
The Store must be strictly inside the canonical workspace directory.

The CLI resolves [revision selectors](doc:native-tags), including native tags,
before preflight. C++ callers can resolve them explicitly with
`resolve_native_revision()`; the checkout API itself retains revision-ID inputs.

## Starting and publishing {#starting-and-publishing}

The initial policy requires every tracked file to match its baseline by complete
content hash and supported executable metadata. An existing staging index blocks
checkout, even if it has no staged changes. Ignored and untracked collisions,
links/junctions, protected paths and ambiguous portable aliases are refused.
Clean tracked case changes and file/directory transitions use the namespace
executor described below. There is no `--force` option.

For a revision change, execution repeats preflight and verifies the complete
source and target closures, including historical parents and file hashes,
before publishing its journal.
Thus a metadata-only plan is insufficient to execute checkout. A clean checkout
whose target already equals the branch head returns `state: "unchanged"` without
creating a transaction. Unrelated untracked and ignored files remain untouched.

For each changed file the engine first reconstructs and verifies the complete
target into a private slot. After rechecking the workspace, it moves the original
to a backup slot and installs the prepared target with exclusive, no-overwrite
moves. Deletions retain their original file in a backup. Representation-only
changes need no file move; mode-only changes currently prepare a complete file.
Executable bits are applied on Unix. Windows preserves their snapshot metadata
without applying it to the filesystem or treating it as a local modification.

All desired tracked files are verified again after the last public progress
callback. One compare-and-swap reference transaction then moves the branch,
marks checkout committed, archives the source and retains the target and receipt.
Only after that publication does the engine remove verified private data and
release the active journal/pending roots. Success returns `state: "committed"`,
`pending: false`, the transaction `id`, and its `archive` reference or null for
an unborn branch.

Publication is atomic for the branch/journal references. **Workspace file moves
are sequential**, so readers can see intermediate contents. Stop external
writers while checking out; this is not an atomic filesystem snapshot, workspace
lock or defense against hostile concurrent namespace replacement. An IO error
or cancellation after journal creation requires explicit recovery.

## Inspecting, resuming and aborting {#inspecting-resuming-and-aborting}

`checkout-status` returns `pending: false` when no operation is active. Otherwise
it reports the immutable transaction identity, branch, source, target, canonical
workspace, state, private `data_directory`, and affected paths with their numeric
slot names. `--id` can inspect the active transaction or a completed receipt.
JSON fields and state names remain English with either interface language.

| State | Resume | Abort |
| --- | --- | --- |
| `preparing` | Finish reconstruction, recheck the clean baseline, then apply | Discard prepared outputs without touching working files |
| `applying` | Validate backups/installed files and finish application | Restore originals in reverse action order |
| `evacuating` | Finish backing up changed sources and removing required empty directories | Uninstall targets, then restore sources |
| `installing` | Validate/install all targets, then publish | Uninstall all targets, then restore sources |
| `uninstalling` | Continue the requested target removal | Continue target removal |
| `restoring` | Continue the requested source restoration | Continue source restoration |
| `aborting` | Continue the requested rollback | Continue rollback |
| `discarding` | Finish discarding preparation | Finish discarding preparation |
| `aborted` | Finish cleanup without replaying file moves | Finish cleanup |
| `committed` | Finish cleanup without changing the published branch | Refused; branch publication already succeeded |

Recovery always uses the recorded workspace/branch; it accepts no override.
Opening a Store does not automatically change its working files. The journal
is bound to the canonical workspace location, so copying or relocating it is
not a supported recovery procedure. A completed recovery is idempotent.

If a working file or private output has changed, recovery preserves those bytes
and refuses the unsafe step. Inspect `checkout-status` and preserve such edits
before retrying. A source edited immediately before its backup move is restored
verbatim during abort. Rollback persists its `aborted` marker before removing
prepared outputs, so interrupted cleanup cannot mistake a later working file
for an installed target. Newly created empty parent directories may remain
after abort. The executor does not recursively remove working directories.

While checkout is pending, GC (including preview) and native commit, add, reset,
staged commit and history detachment are blocked. Packing and read-only
inspection remain available. Generic reference/admin operations are not a
checkout override: changing a branch or internal retention root can make
recovery refuse to proceed. Do not edit or replicate the active checkout roots.

## Guarded recovery by a resident agent {#owned-recovery}

The [bidirectional resident agent](doc:resident-agent) marks its incoming checkouts
and merge checkouts with an explicit owner ID. Journal v11 freezes that owner,
context and both materialization definitions, with layout 1 or 3 for ordinary or
namespace application. The owner participates in the immutable transaction ID.
`resident-sync-v1` is established before durable observations or owned journals;
older readers reject the Store instead of ignoring the new recovery state.
Unowned checkout encodings remain unchanged.

```cpp
store.checkout_directory_owned(project, "main", target, owner_id, operation);
store.recover_checkout_owned(project, "main", owner_id, operation);
```

The recovery method checks the owner, selected context, current physical workspace
and HEAD target before applying anything. The ID is a coordination tag, not an
authentication credential against a trusted local Store writer. An agent also
requires its retained remote observation to be an ancestor of the prepared target.
Another endpoint, workspace or publication-only session leaves it blocked.

Active owned transactions can finish offline without a signing private key: the
exact incoming or signed merge target is already frozen and retained. Recovery
does not compose or re-sign a revision. Starting a new signed operation still
requires the signing identity. Manual or unrelated transactions require the
ordinary explicit `checkout-recover`; opening a Store alone performs no recovery.
Completed owned receipts remain inspectable and retained under the same rules
as other receipts. Backups preserve reader requirements and complete revision
data, without activating old physical checkout state in a restored directory.

## Journal, retention and private data {#journal-retention-and-private-data}

The immutable journal is an ordinary chunk object, so existing compression,
encryption and pack support apply without changing logical/physical formats.
Its identity hashes the version, nonce, canonical directory, branch and source/
target revisions. Switch journals add destination and expected/new HEAD fields
in version 2. Namespace checkout/switch journals use versions 3/4 and an explicit
`namespace_transition: true` flag; versions 1/2 remain supported. Each phase publishes
a new journal object.

[Hard reset](doc:native-hard-reset) uses version 5, which distinguishes the
captured working snapshot from the original branch head/index and records the
source directory profile. Versions 1–4 keep their existing semantics.

| Reference | Retained value |
| --- | --- |
| `checkouts/active` | Current journal object; at most one checkout per Store |
| `checkouts/pending/ID/target` | Complete target closure until cleanup |
| `checkouts/pending/ID/source` | Complete original closure when a source exists |
| `archive/checkout/ID` | Previous head after successful publication |
| `checkouts/completed/ID` | Published target after successful publication |
| `checkouts/receipts/ID` | Committed journal for later inspection |

The active/pending roots are released only after cleanup. Archives, completed
targets and receipts remain retained; automatic pruning is not implemented.
Deleting a branch therefore does not alone discard checkout history. Removing
all roots and running destructive GC intentionally makes those versions
unrecoverable. An aborted transaction leaves no permanent receipt.

Private files live at `STORE/checkouts/ID/{new,old}/N.bin`. **They contain decoded
plaintext even in an encrypted Store**, just as the workspace does. Existing
object encryption does not encrypt these temporary targets or original-file
backups. Unix private directories use owner-only permissions; Windows relies on
the Store's inherited ACLs. Filesystem encryption is needed to protect these
working bytes at rest. Prepared targets require extra disk space; backups are
moved original files. Source, private slots and destinations must support
exclusive moves on the same filesystem. Cross-filesystem moves fail with the
journal retained for recovery.

Cleanup validates known slot names, types and expected hashes before removing
files, and removes only empty directories. Unexpected or changed private data
is retained with an error. Windows may briefly defer deletion of a file; cleanup
retries only nonrecursive empty-directory removal for up to about 500 ms.
Other IO failures leave the journal available for another recovery attempt.

## C++, progress and limits {#c-progress-and-limits}

```cpp
auto result = store.checkout_directory(project, "main", target_id, operation);
auto pending = store.checkout_status();
auto resumed = store.recover_checkout(false, operation);
auto aborted = store.recover_checkout(true, operation);
auto receipt = store.checkout_status(transaction_id);
```

Callbacks are synchronous and must not reenter the Store. Preflight/comparison
use their existing scanning, planning and verification events. Preparation adds
per-file reconstruction; application/rollback report `exporting` with logical
changed-file byte totals, interspersed with per-file verification. Counters can
restart between these phases and on recovery. The last observer before branch
publication is `publishing` with path `checkout`. Cancellation or observer failure
before that commit point keeps the original branch, but may leave a partial
workspace and pending journal. After publication, cleanup finishes without
further observers or cancellation. An error after publication can therefore
report failure with a committed branch; inspect status and resume cleanup.

The shared 1 MiB content buffer, snapshot tables, manifest-summary caches,
16 MiB object limits and bounded workspace inventory still apply. This version
performs complete preflight/final hashes and closure verification, reconstructs
whole changed files, and hashes backups before cleanup. It does not yet use
in-place block patching or parallel materialization; the advisory work index does not replace these checks.
The C ABI and Go binding do not expose native checkout yet.


## Recoverable namespace transitions {#recoverable-namespace-transitions}

Clean tracked files can change case, including parent directories, or cross
file/directory boundaries. For example, `Source/Code.cpp` can become
`source/code.cpp`, a tracked `Content` file can become `Content/Map.uasset`, and
a fully tracked `Content/...` tree can become a `Content` file. Ordinary renamed
paths are already represented as removals/additions; rename detection and
history following remain separate work.

Planning keeps the full clean-tracked policy, checks exact portable directory
spellings, and reports `namespace_transition: true` and `remove_directories` when
the operation requires this executor. Each affected directory must contain only
the source snapshot's files and their known parent directories. Untracked or
ignored descendants, unknown empty directories, links and protected storage
block the entire operation before journal publication. Snapshots with inconsistent
case spellings for the same portable parent are rejected.

The executor prepares every target, then backs up **all changed source files**
before installing any target. It removes only the snapshot-derived directories
needed for the transition, deepest first, and only while empty. There is no
recursive removal. It persists `installing` before creating target parents and
moving the prepared files. Final hashes, exact namespace checks and atomic
branch/archive/journal publication retain the existing guarantees.

Abort first moves installed targets back into their prepared slots, removes
only required empty target directories, persists `restoring`, then restores the
original sources and their spelling. Distinct durable phases prevent resumed
rollback from confusing restored originals with still-installed targets.
Changed working files and newly appeared obstructions are preserved and block
unsafe recovery. Opening the Store still performs no recovery automatically.

Switch, merge, replay and linear rebase use the shared checkout executor; branch
switch preserves both tips and publishes its HEAD descriptor with the journal.
No new object, pointer, revision or network format is introduced. Older engines
cannot recover namespace journals and must reject their unsupported versions.


Version 6 supports [stash application](doc:native-stash): the branch head stays
fixed while the working target, optional frozen index and stash pop are
published together. All previous journal versions remain readable.

[Completed receipt retirement](doc:checkout-retention) removes selected
receipt/target groups only after independent backup coverage. Historical receipt
inspection does not require an obsolete HEAD selection or deliberately retired
source snapshot; active recovery keeps the full binding/snapshot checks.
