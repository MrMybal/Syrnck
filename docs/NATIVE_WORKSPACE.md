# Native workspace inspection and checkout preflight

The native engine provides a combined branch/index/workspace status and a
read-only checkout plan. Neither operation changes files, objects, references
or staging. A plan does not execute a checkout; the separate
[recoverable checkout](NATIVE_CHECKOUT.md) prepares verified outputs, rechecks
the workspace and journals file moves before publishing the branch.

```text
syrnck native status --combined --branch main --progress
syrnck native checkout-plan TARGET_REVISION --branch main --progress-json
syrnck native checkout-plan TARGET_REVISION --verify
```

Both commands default to the [current native branch](NATIVE_BRANCHES.md)
(`main` for legacy Stores) and directory `.`. The Store must be
inside the workspace; checkout planning requires it strictly below the workspace
root. Explicit HEAD binds CLI workspace commands to that canonical root. Output
keys, action names and conflict codes are English in both interface languages.

## Combined status

`native status --combined` returns `version`, `branch`, `clean`, `staged` and
`unstaged`. The nested `staged` report is the existing
[`native staged` report](NATIVE_STAGING.md): its recorded base is compared with
the immutable index. The nested `unstaged` report is a
[working-directory status](NATIVE_COMPARISON.md) against that index. With no
index, the effective comparison is against the branch head; an unborn branch
uses an empty baseline.

A path can appear in both reports. For example, stage `prepared` over baseline
`original`, then restore the working file to `original`. Ordinary branch status
is clean, but combined status reports a staged modification and an unstaged
modification. This keeps the pending commit distinct from later working edits.

An index whose base differs from the current branch remains inspectable against
its recorded base. Working files are still compared with its frozen snapshot.
Overall `clean` requires both comparisons to be clean and the index to be fresh.
Staleness alone therefore makes the combined report unclean. A dirty report
still exits 0. `--combined` cannot be combined with `--revision`.

The working scan retains ordinary status ignore rules, regular-file requirements
and full BLAKE3 hashing. It does not ingest files or audit chunk payloads.

## Checkout plan and verification

`native checkout-plan TARGET` compares the current `heads/BRANCH` snapshot with
an exact target revision. It inventories the actual workspace and hashes every
tracked baseline file in full. Timestamps and sizes never establish equality.
The first policy, `clean-tracked-v1`, requires **all tracked files to be clean**,
including files whose target entry would remain unchanged. Any explicit staging
index blocks this preflight, including an index without staged changes.

The plan includes:

| Field | Meaning |
| --- | --- |
| `source`, `target` | Baseline revision (or null) and exact target revision |
| `staging_snapshot` | Existing index ID, or null |
| `policy`, `ready` | Preflight policy and whether it found no blockers |
| `objects_verified` | Whether complete target closure verification was requested and succeeded |
| `executable_metadata_supported` | False on Windows; executable bits cannot be inspected/applied there |
| `inventory_entries` | Files, directories and opaque/protected entries inventoried |
| `hashed_bytes` | Working baseline file bytes read, excluding stored-object verification |
| `restore_bytes` | Logical target bytes for proposed create/replace actions |
| `counts`, `actions` | Proposed snapshot changes and unchanged count |
| `create_directories` | Missing target parent directories, sorted |
| `conflicts` | Sorted unique blockers with `path`, `reason` and `obstruction` |

Actions are sorted by repository path. Each action contains `source`/`target`
file summaries with manifest, size, file hash and executable metadata. Missing
sides are null. These actions describe the candidate snapshot even if conflicts
block it; they are not an ordered transaction program.

| Action | Meaning |
| --- | --- |
| `create` | Target file has no baseline entry |
| `replace` | Existing file has different target contents |
| `remove` | Baseline file has no target entry |
| `set_executable` | Contents match and executable metadata differs |
| `reuse` | Contents/mode match but manifest representation differs |
| `unchanged` count | File entries match; no action row |

`restore_bytes` is a file-level logical count, not network traffic or unique
chunk bytes. [Manifest reuse plans](FILE_COMPARISON.md) provide block-level
comparison separately. Windows plans retain desired executable changes in the
snapshot comparison, but do not use local executable bits as a cleanliness test.

Default planning requires baseline/target revision and file-manifest metadata.
It reads no target chunk payloads or historical parents and can plan from a
metadata-only partial Store. Thus `ready: true, objects_verified: false` means
the filesystem preflight passed; it does not prove reconstruction is possible.
`--verify` additionally verifies the target's entire closure, including parents
and file hashes, as ordinary native export does. Missing/corrupt dependencies
fail the command without returning a partial plan. Verification still runs when
the filesystem report contains blockers.

A completed ready plan exits 0. A completed blocked plan emits its JSON and
exits 1. Invalid arguments, IO errors, missing required metadata or verification
errors emit a diagnostic on stderr and no JSON report.

## Protected paths and first-policy limits

Unlike ordinary status, checkout preflight does **not** apply `.syrnckignore`.
Ignored files are inventoried so they cannot hide a local edit or collision.
Unrelated untracked/ignored files are preserved by the proposed action list;
their contents are not hashed. A pre-existing untracked target file blocks the
plan even if its bytes happen to match the desired target.

| Conflict reason | Meaning |
| --- | --- |
| `tracked_modified`, `tracked_missing` | Local tracked edit/deletion |
| `tracked_mode_changed` | Local Unix executable metadata differs |
| `staging_present`, `stale_staging` | The branch has a pending or stale index; path is empty |
| `checkout_pending` | A checkout journal requires recovery; path is empty |
| `untracked_collision` | A target would occupy an untracked path, or an affected directory contains untracked/ignored entries |
| `file_directory_transition` | A file/directory conversion or ancestor obstruction includes an entry outside the tracked transition |
| `path_spelling` | A name matches only after Unicode normalization/case folding |
| `portable_collision` | Multiple physical names fold to the same affected name |
| `symlink_obstruction`, `special_file` | An affected path or ancestor is not an ordinary file/directory |
| `protected_path` | The operation would enter the Store or another protected entry |

Static symlinks and Windows reparse points, including junctions, are opaque.
The inventory never deliberately traverses them. `.git` and `.syrnck` names and
the actual Store directory are protected and their contents are excluded from
the workspace inventory. Custom Store names receive the same protection.
Clean tracked case/normalization changes and file/directory transitions are
planned through the [recoverable namespace executor](NATIVE_CHECKOUT.md#recoverable-namespace-transitions).
Such plans add `namespace_transition: true` and `remove_directories`. Only
snapshot-owned files and their known parent directories can be displaced;
unknown empty directories and ignored descendants remain protected. Parent
spellings inside each snapshot must be consistent under portable folding.
No force, overwrite, automatic index reset or adoption option is implemented.

## C++, progress and resource boundaries

```cpp
auto status = store.workspace_status(project, "main", operation);
auto preview = store.plan_checkout(project, "main", target_id);
auto verified = store.plan_checkout(project, "main", target_id, true, operation);
```

All callbacks are synchronous on the caller and must not reenter the Store.
`scanning` reports directories/inventory entries, `verifying` reports working
file bytes or stored closure progress, and `planning` reports metadata/action
entries. Counters reset between phases. Planning finishes with a complete
`planning` event and never emits a `publishing` event. Cancellation or observer
failure returns no partial report and changes no data. The ordinary exclusive
Store lock and Store opening/recovery rules still apply.

The working content buffer is 1 MiB. Each snapshot load uses a FIFO cache of up
to 1,024 manifest summaries. Physical inventory has a 16 MiB entry accounting
budget (`64 + UTF-8 path bytes + folded path bytes` per entry) and a maximum
directory depth of 128. This accounting is not a total RAM bound: maps, reports,
JSON and allocation overhead are additional. Existing 16 MiB object limits
and full-closure verification collections also remain.

These are observations of a changing filesystem, not an atomic snapshot or a
workspace lock. Size/type/time/mode changes observed around a file read cause
failure; later edits can invalidate a completed report. The executor must
prepare verified contents, revalidate its expectations and use a recovery
journal. Neither `ready` nor a prior plan authorizes it to skip those steps.
The C ABI and Go binding do not yet expose native workspace operations.

Regression tests cover combined edits on both sides of one path, stale/clean
indexes, unborn branches, action counts, rechunking, executable changes,
equal-size/equal-time edits, ignored collisions, protected custom Stores,
portable aliases, directory obstructions, Windows junctions, metadata-only
Stores, verification errors, cancellation, callback errors, source mutation
and byte-for-byte read-only snapshots. Plain/encrypted and loose/single-pack/
multi-pack variants run locally on Windows x64; Unix paths await CI execution.
