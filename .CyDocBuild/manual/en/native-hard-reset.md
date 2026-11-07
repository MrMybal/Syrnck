---
id: native-hard-reset
title: Recoverable native hard reset
description: Recoverable native hard reset guide.
section: Version control
order: 260
parent: native-history
revision: 1
---

# Recoverable native hard reset {#recoverable-native-hard-reset}

Hard reset replaces the selected branch's tracked workspace and clears its
frozen staging index. Before replacing local data, it captures complete file
contents and retains the original head and index through a recovery snapshot.

```text
syrnck native reset-plan TARGET --hard --verify
syrnck native reset TARGET --hard --expect OLD_HEAD --expect-plan PLAN_ID
syrnck native reset HEAD --hard
```

`--directory` selects the workspace. The Store must be below that workspace,
and `--branch` must identify its currently selected branch. Existing workspace
binding checks apply. A pending checkout/switch/reset must first be recovered
or aborted. Soft/mixed reset and staging-only reset retain their existing APIs.

## What is replaced and protected {#what-is-replaced-and-protected}

The read-only plan inventories the namespace and hashes the actual selected
working files, independently of ignore rules, file size and modification time.
It includes:

- Present paths tracked by the current head or frozen staging snapshot.
- Untracked regular files obstructing a target file/directory or spelling change.
- Observed directories needed to remove and reverse those namespace transitions,
  including empty directories.

Missing tracked files are represented by their absence, then restored when the
target contains them. Modified tracked files, staged additions/deletions and
stale frozen indexes can be reset. Case changes and file/directory transitions
use the existing evacuation/install journal. Untracked files outside those
obstructions remain in place. The plan lists `obstructing_untracked` paths
explicitly; those files leave the working tree but remain reconstructible from
the recovery snapshot.

Links/reparse points, special files, ambiguous portable names and protected
`.git`/Store entries are not adopted. Such obstructions block reset before
candidate creation. Directory removal is nonrecursive and requires emptiness
after the captured regular files have been evacuated.

`plan` binds the canonical workspace and Store locations, selected HEAD,
original head/index, target, exact working path spellings/content hashes/modes
and source directory profile. Restoring a file's old size and timestamp does
not bypass this fence. `--verify` additionally checks the complete retained
head/index/target closures and reconstructed file hashes; it does not change
the plan ID. A C++ application must supply the plan ID. The CLI can capture a
fresh plan immediately before application when `--expect-plan` is omitted.

## Application and recovery {#application-and-recovery}

Application verifies all old retained versions and the target before importing
new working contents. Each imported file must still match the planned complete
hash, size and mode. A canonical recovery revision stores the captured files
and has the original head and optional index as parents. This preserves both
the dirty working bytes and independently staged bytes through later GC.

The version 5 checkout journal records separate working-source and branch-head
identities, the original staging snapshot, HEAD selection and the observed
directory profile. Initial publication pins the complete source and target
before working files move. Final publication updates the branch, removes the
index and creates the recovery archive in one reference transaction.

```text
syrnck native checkout-status
syrnck native checkout-recover
syrnck native checkout-recover --abort
```

Recovery resumes installation or cleanup. Abort before final reference
publication restores pre-reset working bytes, missing-file state, directory
spellings and captured empty directories; the head and index remain unchanged.
After publication, recovery finishes cleanup. Store opening performs no implicit
workspace recovery. Concurrently changed or newly obstructed paths cause a
refusal that preserves the observed data for explicit recovery.

Successful JSON includes `operation: reset`, `mode: hard`, the previous revision
and index, `recovery_revision`, `archive`, `plan` and the normal checkout receipt.
The archive points to the recovery revision. Its parents keep the entire old
history and frozen index reconstructible independently of branch retention.
For example:

```text
syrnck native export RECOVERY_REVISION saved-working-files
syrnck native export PREVIOUS_INDEX saved-staged-files
syrnck native archives --kind checkout --verify
```

Revision export restores files; empty-directory metadata is part of the journal
and is used for abort, rather than tracked in flat revision objects. Completed
archives remain retained until explicitly retired with verified backup coverage.
A clean same-head reset with no index returns `state: unchanged` without writes.
Preparation failures can leave valid unreferenced CAS objects; a published
journal must be recovered before GC. Checkout prepared files/backups remain
plaintext, as in [ordinary checkout](doc:native-checkout).

## API, bounds and validation {#api-bounds-and-validation}

The core-only C++ APIs are declared in `syrnck/workspace.hpp`:

```cpp
auto plan = syrnck::plan_native_hard_reset(store, project, "main", target, true);
auto applied = syrnck::native_hard_reset(store, project, "main", expected_head,
                                         target, plan["plan"].get<std::string>());
```

Flat revision metadata, reports, source profile and inventory each retain their
16 MiB budgets. At most 100,000 source-directory entries are accepted and
inventory traversal remains limited to 128 directory levels. File bytes use
the configured streaming ingestion pipeline; the operation still inventories
and hashes its selected working files. This is separate from a watcher/index.
