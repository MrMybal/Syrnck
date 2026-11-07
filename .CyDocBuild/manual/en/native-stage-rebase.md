---
id: native-stage-rebase
title: Rebase a native staging index
description: Rebase a native staging index guide.
section: Version control
order: 370
parent: native-history
revision: 1
---

# Rebase a native staging index {#rebase-a-native-staging-index}

`native rebase-index-plan` replays a frozen index's changes onto its branch's
current head. It reads stored snapshots and manifest metadata, without scanning,
ingesting or modifying working files. Dirty, ignored and untracked working files
do not affect the plan. This operates on native staging, independently of Git's index.

```text
syrnck native staged
syrnck native rebase-index-plan --verify
syrnck native rebase-index-plan --branch feature
```

## Replay and preview {#replay-and-preview}

The recorded staging parent is the baseline, the current branch head is current,
and the complete frozen index is incoming. Independent file and executable-mode
changes combine through the shared three-way file merger. Content, add/delete,
case and file/directory collisions block the result. Optional text composition is
described below; rename detection remains future work. A pending checkout/switch
journal also blocks the plan.

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
Without text composition, metadata-only plans do not audit block payloads.
Text-enabled plans reconstruct eligible unresolved content files and verify their
hashes even without `--verify`. Reports and candidate revision
metadata have the existing 16 MiB bounds; snapshots are still flat file maps.
The report includes an empty `resolution_context` and accepted `resolved` rows.

```cpp
auto plan = store.plan_rebase_stage("feature", true, operation);
```

The API supports cooperative cancellation and progress. Owned core diagnostics
are English; CLI help, errors and progress support English and French. JSON keys
and reason codes remain English. Callbacks must not reenter the Store. Native
history C/Go bindings remain future work.

## Apply and retain prepared bytes {#apply-and-retain-prepared-bytes}

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


## Resolve index conflicts {#resolve-index-conflicts}

Both commands accept `--resolutions JSON_FILE`, using the shared
[file resolution schema](doc:native-resolutions). Copy the plan's
`resolution_context` into a JSON file and fill its `files` map. Its operation is
`rebase-index`, `head` is the current branch tip, `source` is the frozen index,
and `base`/`parent` are both its recorded baseline (null for an initial index).
All IDs must match when planning/application runs. Changed source indexes or
branch tips require a new plan and choices.

```text
syrnck native rebase-index-plan --resolutions choices.json --verify
syrnck native rebase-index --resolutions choices.json --expect HEAD_ID --expect-index INDEX_ID
```

Choices select a complete entry from `current` (the new branch head), `incoming`
(the old prepared index), `base`, `remove`, or a custom `file` manifest with an
explicit executable mode. Only original conflict paths may have choices.
Namespace collisions are checked again after applying them. Incomplete choices
remain blocked and publish nothing. Non-conflicting paths cannot be overridden
through a resolution file. Empty choices on a fresh index keep its exact ID.

Raw decoding rejects duplicate JSON fields, unknown fields, invalid paths/IDs and
malformed choices, with the existing 16 MiB, 100,000-path and depth/field limits.
Custom manifests are checked by metadata-only planning; `--verify` and application
audit their payloads as well. Pin imported files if GC may run before application
retains them. A successfully published index protects their blocks; a later staged
commit retains the selected contents in the ordinary revision chain.

```cpp
auto choices = syrnck::NativeResolutionSet::parse(plan["resolution_context"]);
choices.files["Content/Map.uasset"] = {syrnck::NativeResolutionChoice::incoming};
auto preview = store.plan_rebase_stage_resolved("feature", choices, true, operation);
auto result = store.rebase_stage_resolved("feature", expected_head, expected_index,
                                          choices, operation);
```


## Optional bounded text composition {#optional-bounded-text-composition}

```text
syrnck native rebase-index-plan --text-merge --verify
syrnck native rebase-index --text-merge --expect HEAD_ID --expect-index INDEX_ID
```

`--text-merge` composes independent UTF-8 edits between the recorded staging base,
current head and frozen index. It never reads working text; later dirty bytes stay
untouched. Explicit `--resolutions` choices take priority. Remaining binary,
overlapping, add/delete and namespace conflicts block the entire publication.
Line endings and missing final newlines are preserved, and executable mode is
combined independently.

The [native text batch limits](doc:native-merge#opt-in-text-composition) apply:
64 attempts and 16 MiB total input plus clean result bytes, with the usual
`--max-file-bytes`, `--max-lines`, `--max-work` and `--max-output-bytes` per-file
limits. Budget flags require `--text-merge`. Reports add `text_merge`, including
status/reason/coordinates and proposed manifest metadata per attempted path.
They include no merged plaintext. An already fresh index preserves its exact ID
and reports zero text attempts.

Planning reconstructs only eligible unresolved content files, verifies their
hashes, and predicts canonical CDC manifest IDs without writing objects. Apply
verifies all retained inputs before materializing generated data and creating the
candidate. The old index archive and new staging reference publish in one expected
head/index transaction. A conflict publishes neither. Cancellation before that
transaction can leave complete unreferenced objects while references and working
files remain unchanged. The archived old index and its base remain reconstructible
after GC; a later staged commit retains the complete merged snapshot.

```cpp
syrnck::NativeStageRebaseOptions settings;
settings.text_merge.emplace();
auto plan = store.plan_rebase_stage_with_options("feature", settings, true);
auto result = store.rebase_stage_with_options("feature", expected_head,
                                               expected_index, settings);
```

The named options methods preserve compatibility with existing plain/resolved
entry points.
