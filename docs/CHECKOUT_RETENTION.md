# Completed checkout receipt retirement

Completed checkouts retain both an immutable receipt and a target revision root.
Their source archive independently protects the previous files. These roots also
cover switch, hard reset and stash journals. They deliberately survive normal GC.
The API in `syrnck/checkout_retention.hpp` provides paginated inspection and
explicit, backup-fenced retirement.

```text
syrnck native checkout-receipts --limit 100 --verify
syrnck native checkout-receipts --after TRANSACTION_ID --limit 100
syrnck backup create /existing-backup-parent/before-retirement
syrnck backup checkout-retire-plan TRANSACTION_ID --from /existing-backup-parent/before-retirement
syrnck backup checkout-retire TRANSACTION_ID --from /existing-backup-parent/before-retirement --expect-plan PLAN_ID
```

The default removes `checkouts/receipts/ID` and `checkouts/completed/ID` together.
Add `--include-source-archives` to both commands to also retire that transaction's
`archive/checkout/ID` root in the same reference transaction. No file bytes,
working files, branch heads, staging indexes or private recovery directories are
removed. Later GC remains an independent, explicit operation.

## Coverage and fences

Select 1..1024 unique canonical transaction IDs. The plan binds the source Store
location, complete reference table, selected receipt/target/source pairs, archive
option and exact verified backup snapshot. A changed selection, source refs,
backup or option invalidates the plan.

The separate backup must pass full descriptor, inventory and file verification.
It must reconstruct every selected target, and each source archive being removed,
with all revision parents, manifests and payloads. Receipt metadata contains local
workspace paths and is intentionally omitted by backup; the portable revision
roots remain covered. Such restored Stores expose entries with
`retained_only: true`, which can also be retired with backup coverage.

Pending checkouts block inspection/retirement. Orphan pending pins, contradictory
receipt/target/source pairs, linked checkout directories and any remaining private
recovery data cause refusal. Retirement never cleans or adopts unknown files.
Finish the existing checkout's normal recovery first.

Application runs its final progress observer before revalidating the complete
backup and exact plan. Final verification and the CAS use cancellation only,
without further observer callbacks. A callback that changes source refs or the
backup therefore cannot retire roots using stale evidence. Cancellation before
publication preserves all selected references. A process failure sees the entire
old or new group; rerun inspection/planning after a lost command response.

The primary Store uses `SYRNCK_KEY`; the backup uses `SYRNCK_BACKUP_KEY`. Keep the
backup and its key under an appropriate retention policy. Other pins, branches,
stashes or archives may still retain the same versions, so retirement does not
promise immediate disk savings. A later backup deletion remains an administrator's
responsibility.

## Historical receipts

`native checkout-status --id ID` validates the immutable historical record without
requiring an old symbolic HEAD selection or an explicitly retired source snapshot
to remain live. Those objects are required when **actively recovering** a journal,
where their binding and snapshot validation still run. Historical receipt IDs and
record hashes remain checked. If its source archive was retired and the source
revision was collected, status returns `files: null` and `files_available: false`
instead of inventing file actions. File reconstruction uses retained revision roots or
the verified backup; a receipt alone does not keep retired file bytes reachable.

After receipt retirement, querying that receipt returns unknown transaction. The
backup retains its source/target files, while normal backup restore intentionally
does not recreate local workspace receipts.

## Validation and limits

Pages contain at most 1000 entries. The reference table and retirement reports
retain their 16 MiB limits. Inspection can audit complete displayed closures with
`--verify` without writing files. File payload verification uses the existing
streaming engine; the current graph traversal still keeps object identities in
memory.

Tests exercise plaintext/encrypted and loose/single-pack/multi-pack Stores,
independent dirty/index/head versions, source inclusion/exclusion, restored
receiptless roots, stale plans, cancellation, observer mutations, foreign private
data, missing coverage, GC and historical receipts after old HEAD/source collection.
A private IO probe tests process termination around atomic reference publication,
then a fresh production reader audits and resumes retirement (40 boundaries in
the local campaign). Cross-platform and
power-loss validation remain separate work.
