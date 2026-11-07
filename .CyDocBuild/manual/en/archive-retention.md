---
id: archive-retention
title: Retire native archives with a verified backup
description: Retire native archives with a verified backup guide.
section: Storage
order: 20
parent: storage
revision: 1
---

# Retire native archives with a verified backup {#retire-native-archives-with-a-verified-backup}

Native archives deliberately retain old revision closures after reset, rebase,
checkout, amend and index rewrites. Immutable `native-fetch/REVISION` roots also
retain fetched history. GC keeps those versions until their roots are explicitly
retired.

The C++ API in `syrnck/retention.hpp` and the `backup retire-plan` / `backup retire`
commands retire selected simple archive roots and complete branch/tag archive
groups in one reference transaction.
A separate verified [backup](doc:backups) must reconstruct every selected
revision, including every parent revision and complete file payload.

```text
syrnck native archives --kind reset
syrnck backup create /existing-backup-parent/before-retirement
syrnck backup retire-plan archive/reset/REVISION --from /existing-backup-parent/before-retirement
syrnck backup retire archive/reset/REVISION --from /existing-backup-parent/before-retirement --expect-plan PLAN_ID
syrnck gc
```

Use the exact `plan` value from the read-only preview. The plan binds the complete
current reference table, sorted selected references and verified backup snapshot.
An intervening reference change or a different backup invalidates it. The apply
command recomputes coverage and performs one compare-and-swap transaction.
Cancellation before publication leaves all selected roots in place. It does not
delete payloads or run GC; inspect a later GC preview separately.

Supported roots are the simple revision archives recognized by `native archives`:
`reset`, `reset-index`, `rebase-index`, `stage-index`, `squash`, `amend`, `checkout`,
`fetch`, `publish`, `bisect`, `stash` and detached-history archives. Exact `native-fetch/REVISION` roots are
also supported. Select 1..1024 unique references. Heads, tags, indexes,
and asset-lock roots are refused. Structured archives use whole-group selectors:

| Selector | Roots removed together |
| --- | --- |
| `archive/branches/ID` | Descriptor, target revision and optional frozen index |
| `archive/tags/ID` | Descriptor, target revision and optional annotation |

Discover IDs with `native branch-archives` and `native tag-archives`; both accept
`--after ID`, `--limit N` and `--verify`. Inspect a particular archive before
retiring it. A member such as `archive/branches/ID/index` or
`archive/tags/ID/target` cannot be retired independently. Incomplete groups,
unexpected members, mismatched annotation bindings and invalid staging baselines
are refused. The verified backup must contain the exact complete group under its
original reference names; the same payload objects under other names do not
provide a recoverable archive recipe.

```text
syrnck native branch-archives --verify
syrnck native tag-archives --verify
syrnck backup create /existing-backup-parent/before-retirement
syrnck backup retire-plan archive/branches/BRANCH_ID archive/tags/TAG_ID --from /existing-backup-parent/before-retirement
syrnck backup retire archive/branches/BRANCH_ID archive/tags/TAG_ID --from /existing-backup-parent/before-retirement --expect-plan PLAN_ID
```

Group rows add `name` and a `roots` map to the preview's `references` array.
`kind` is `branch-group` or `tag-group`; `revision` is the archived tip.
`removed_references` counts the expanded roots, including descriptors and optional
index/annotation roots. The limit of 1..1,024 applies to unique selectors. Reports
are bounded below 16 MiB including JSON escaping. Simple roots and groups can be
mixed in one atomic transaction. Recreated branches/tags with the same original
names, current HEAD, working files and unrelated roots remain untouched.

The primary Store uses `SYRNCK_KEY`. The secondary backup uses
`SYRNCK_BACKUP_KEY`, or no key if that variable is absent. The two Stores can
have independent envelope keys. The backup remains unchanged and exclusively
locked during verification and publication. Native checkout recovery must be
finished before retirement.

Other local roots may still retain the selected revision after its archive is
removed. Retirement therefore does not promise immediate disk savings. When
the local version becomes unreachable and is later collected, its verified
backup remains the recovery source. Keep that backup under an appropriate
retention policy; this operation does not prevent a future administrator from
deleting it. Restore that backup to a separate Store with `backup restore`, then
use `native branch-restore ID` or `native tag-restore ID` there. Restoring directly
inside a verified backup changes its references and invalidates its descriptor.
Automatic age/release policies and distributed GC remain future work.


[Completed checkout retirement](doc:checkout-retention) handles receipt/target
pairs and optional checkout-source archives together. It also supports retained
target roots restored from a backup without local workspace receipts.

Apply runs its final progress observer before revalidating the backup and exact
plan. Final verification/publication uses cancellation without further callbacks,
so observer mutations of source refs or backup evidence cannot authorize stale
retirement.
