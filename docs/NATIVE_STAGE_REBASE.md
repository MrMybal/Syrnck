# Rebase a native staging index

`native rebase-index-plan` replays a frozen index's changes onto its branch's
current head. It reads stored snapshots and manifest metadata, without scanning,
ingesting or modifying working files. Dirty, ignored and untracked working files
do not affect the plan. This operates on native staging, independently of Git's index.

```text
syrnck native staged
syrnck native rebase-index-plan --verify
syrnck native rebase-index-plan --branch feature
```

## Replay and preview

The recorded staging parent is the baseline, the current branch head is current,
and the complete frozen index is incoming. Independent file and executable-mode
changes combine through the shared three-way file merger. Content, add/delete,
case and file/directory collisions block the result. There is no automatic text
merge or rename detection. A pending checkout/switch journal also blocks the plan.

The branch must have a head and an index. An index prepared before the first
branch commit can be replayed with an empty baseline after a head exists. There
is no ancestor requirement: the new head is an explicit destination. The CLI
defaults to the selected native branch; `--branch` does not change that selection.

An already fresh index is unchanged, preserving its exact ID and prepared file
entries. A stale index proposes an ordinary v1 staging snapshot with the current
head as its parent, the branch's staging marker and timestamp zero. Empty results
remain explicit indexes. The proposed ID is deterministic and is not written by
planning; it need not yet be inspectable in the Store.

Logical content and executable changes follow the ordinary replay policy. An
explicitly prepared representation-only change is also retained when the new
head still has the baseline's content and manifest; independent mode changes can
combine with it. If upstream replaced the representation too, upstream wins when
the logical contents match. A changed upstream file cannot keep an obsolete
prepared representation of its old contents.

Reports contain `head`, original `index` and nullable `base`, `stale`, `changed`,
proposed `target`, `archive`, `ready`, `objects_verified`, `result_files`, `empty`,
`counts`, `changes`, `conflicts`, `resolution_context` and `blockers`. Change rows
compare the proposed index with the current head and distinguish content, mode
and manifest differences. For a blocked result they describe a provisional merge;
`target` and `result_files` are null. A ready plan exits 0; a blocked plan exits 1
with JSON on stdout. Invalid or missing inputs produce diagnostics.

Plans do not write candidate objects or references. `--verify` additionally audits
both complete retained closures, including historical reconstructed file hashes.
Metadata-only plans do not audit block payloads. Reports and candidate revision
metadata have the existing 16 MiB bounds; snapshots are still flat file maps.

```cpp
auto plan = store.plan_rebase_stage("feature", true, operation);
```

The API supports cooperative cancellation and progress. Owned core diagnostics
are English; CLI help, errors and progress support English and French. JSON keys
and reason codes remain English. Callbacks must not reenter the Store. Native
history C/Go bindings remain future work.
