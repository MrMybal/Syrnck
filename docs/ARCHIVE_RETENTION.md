# Retire native archives with a verified backup

Native archives deliberately retain old revision closures after reset, rebase,
checkout, amend and index rewrites. Immutable `native-fetch/REVISION` roots also
retain fetched history. GC keeps those versions until their roots are explicitly
retired.

The C++ API in `syrnck/retention.hpp` and the `backup retire-plan` / `backup retire`
commands retire selected simple archive roots in one reference transaction.
A separate verified [backup](BACKUPS.md) must reconstruct every selected
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
`fetch`, `publish`, `bisect` and detached-history archives. Exact `native-fetch/REVISION` roots are
also supported. Select 1..1024 unique references. Heads, tags, indexes,
asset-lock roots and multi-reference tag/branch archive records are refused.
Those structured records need separate retirement workflows.

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
deleting it. Automatic age/release policies, distributed GC and safe retirement
of structured archives remain future work.

Tests cover all six combinations of plaintext/encrypted and
loose/single-pack/multi-pack source storage, no-write planning, stale plans,
cancelled publication, missing backup coverage and reconstruction from the
unchanged backup after local GC.
