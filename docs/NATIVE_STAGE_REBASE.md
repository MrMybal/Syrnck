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

## Apply and retain prepared bytes

```text
syrnck native rebase-index --expect HEAD_ID --expect-index INDEX_ID
syrnck native staged
syrnck native commit --staged -m "Publish the rebased selection"
syrnck native archives --kind rebase-index --verify
```

Application verifies the complete ready plan before creating the replacement
index. Its ID must match the preview. One reference transaction checks the
expected branch and index, retains the old index under
`archive/rebase-index/INDEX_ID`, and replaces `staging/BRANCH`. The branch tip,
symbolic HEAD and working files stay unchanged. Both expected IDs are exact;
the CLI captures omitted values under the exclusive Store lock. C++ requires
them explicitly. Conflicts or a pending checkout block application.

A fresh index returns `state: unchanged` without writes, while still auditing
retained bytes. A changed index returns `state: applied`, `previous_index` and
the new `index`, alongside the original plan fields. `changed`/`stale` describe
the input index; `native staged` describes the resulting fresh index. A result
equal to the current head stays an explicit clean index; staged commit continues
to require changed entries.

Cancellation, missing retained blocks, callback failure or an archive collision
before publication preserve the old references. Candidate creation can leave
complete unreferenced metadata, reusable on retry or collectible by GC. There is
no working-file journal because this operation does not change working files.
Publication uses the existing atomic reference writer, with no callbacks or
cancellation checks after its commit point. An interrupted operation leaves the
old or the complete new root set. Inspect `native staged` before retrying.

The archived index retains all prepared manifests, blocks and its original
baseline history through GC, even after committing or clearing the replacement
index. Archives have no automatic expiry. The ordinary revision/reference formats
and encryption-at-rest behavior are unchanged.

```cpp
auto result = store.rebase_stage("feature", expected_head, expected_index, operation);
```

Tests cover preview/application identity, frozen selections, deletion, unrelated
upstream additions, empty initial baselines, representation-only choices, dirty
working files, exact expected IDs, fresh no-ops, publication cancellation and
observer failures, staged recommit, archive inspection and GC. All storage
variants use the same publication mechanism.
