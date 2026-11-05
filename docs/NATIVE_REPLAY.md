# Native cherry-pick and revert

The replay planner applies a selected commit's file-level delta to a branch
snapshot. Cherry-pick applies parent -> source; revert applies source -> parent.
The target branch can contain later independent changes. All planning is read-only.

```text
syrnck native cherry-pick-plan tags/fix --verify
syrnck native revert-plan COMMIT_ID --branch main --directory .
syrnck native revert-plan MERGE_ID --mainline 2
```

`--mainline N` selects a 1-based parent. Merge revisions require it explicitly;
an ordinary commit defaults to parent 1. A root revision uses an empty tree and
does not accept a mainline option. The existing native selectors work in the CLI;
the C++ API takes exact IDs. A plan prints JSON and exits 0 when ready, or 1 with
the same JSON when empty, conflicted or blocked. Invalid arguments and damaged
required metadata instead produce diagnostics on stderr.

## Three-way decisions

The planner compares the delta's baseline, current branch and incoming snapshot
by complete logical file hash, size and executable metadata. Rechunking the same
bytes is not a content edit. Identical changes or an unchanged incoming file
preserve the current manifest. A change on only one side is selected. Independent
content and executable-mode changes on the same existing file can combine.

Different concurrent file contents produce `content` conflicts. Other reasons
are `add_add`, `delete_modify` and `modify_delete`. Conflicting text or binary
content can be resolved with `--resolutions JSON_FILE`; see the shared
[resolution schema and workflow](NATIVE_RESOLUTIONS.md). Bounded text composition
can be enabled in the options-based planner described below. Rename detection
remains future work. Independent additions are also
checked together for portable `case_collision` and `file_directory_collision`.

The report contains the operation, branch, expected `head`, selected `source`,
`parent`/`mainline`, `ready`, `empty`, `objects_verified`, `result_files`, proposed
`changes`, `conflicts`, accepted `resolved` choices and `resolution_context`.
Changed entries include complete manifest/hash/size/mode
summaries. Conflict entries show baseline, current and incoming summaries; path
collision entries identify the other path. Unresolved conflicts prevent application of the
entire result, including otherwise compatible files.

When snapshot conflicts are absent, `workspace` contains the ordinary
[checkout preflight](NATIVE_WORKSPACE.md). It checks all tracked working files,
ignored/untracked obstructions, protected paths, links, portable collisions,
staging and pending checkout state. Its `target` is null because the candidate
revision has not been written. A clean result with no logical changes has
`empty: true` and is not ready. With snapshot conflicts, `workspace` is null;
no partial candidate is treated as a valid checkout destination.

`--verify` additionally verifies both input histories' complete closures and
reconstructed file hashes. Without it, the planner reads revision/manifest
metadata and hashes working files, but does not verify incoming/custom block payloads.
These read-only queries can still detect missing required metadata.

The compact serialized report is limited to 16 MiB. Candidate file metadata also
has a conservative limit leaving 4 KiB for the revision header/message. Memory
scales with the three input snapshots, result and bounded report; this is not a
streaming tree format. User-supplied long messages remain subject to the ordinary
revision object limit when a result is later committed.

```cpp
auto plan = store.plan_replay(project, "main", selected_id,
                              syrnck::NativeReplayKind::cherry_pick, {}, true, operation);
auto inverse = store.plan_replay(project, "main", merge_id,
                                 syrnck::NativeReplayKind::revert, 2, false, operation);
```

Callbacks must not reenter the Store. Progress and cooperative cancellation use
the existing English/French interface; report fields and conflict reason codes
are language-independent. C ABI and Go replay bindings remain future work.

## Optional bounded text composition

`plan_replay_with_options` accepts a `NativeReplayOptions` value containing
optional `mainline`, `resolutions` and `text_merge` settings. Existing replay
entry points keep their defaults and remain source compatible. With no
`text_merge`, file-level behavior is unchanged.

```cpp
syrnck::NativeReplayOptions settings;
settings.text_merge.emplace(); // Default per-file text limits.
// settings.mainline = 2; // Required for a selected merge parent.
auto plan = store.plan_replay_with_options(project, "main", source_id,
    syrnck::NativeReplayKind::cherry_pick, settings, true, operation);
```

Explicit file choices are applied first. Only remaining existing-file `content`
conflicts are candidates for the [bounded text engine](TEXT_MERGE.md).
Cherry-pick uses the selected parent as text base and source as incoming;
revert uses source as text base and the selected parent as incoming. Both use
the current branch content as the current side. This direction preserves later
independent changes when removing a selected commit's delta. Root, add/delete,
namespace, binary and overlapping cases keep their existing conflict rules.

The `text_merge` report is null when disabled. When enabled it contains the
same limits, counters and per-path status/ranges/target metadata as native merge.
Each attempted file reconstructs and hash-verifies all three inputs, even without
full-history verification. File-size and aggregate limits are checked before
payload reads. At most 64 conflicts are attempted and verified inputs plus clean
results charge at most 16 MiB, in addition to the per-file text limits.

Clean results have exact predicted v1 manifest IDs and internal candidate bytes;
planning writes no objects or workspace files and reports no merged plaintext.
Explicit all-current resolution can still be empty, which never becomes ready.
One unresolved file blocks the entire replay. Existing workspace, staging,
pending-journal, mainline and resolution-context checks still apply.

`replay_directory_with_options` applies the same settings after checking the
exact expected head. It recomputes the complete plan and verifies both input
closures before materializing any generated bytes. Empty, conflicted, staged or
workspace-blocked operations do not materialize text. Generated manifests must
match their predicted IDs before the single-parent candidate is written.

```cpp
auto applied = store.replay_directory_with_options(project, "main", expected_id,
    source_id, syrnck::NativeReplayKind::revert, "Undo selected changes",
    settings, operation);
```

The normal checkout journal retains the complete candidate, including generated
manifests, for resume or abort. Cancellation before journal creation may leave
unreferenced complete objects but cannot move the branch or edit working files.
Once published, the resulting file's manifest directly retains all of its blocks;
reconstructing it does not require the unrelated source's original history.
Retain a source branch/tag/pin when that history should also survive GC.

## Apply a planned change

```text
syrnck native cherry-pick SOURCE_ID -m "Apply the asset fix" --expect OLD_TIP_ID
syrnck native revert SOURCE_ID -m "Undo the asset fix"
syrnck native revert MERGE_ID --mainline 2 -m "Undo the selected merge delta"
```

`-m`/`--message` is required, as for ordinary native commits. Branch/directory
defaults and selectors match the planner. `--expect`, when supplied, must be an
exact lowercase 64-character old tip ID. Otherwise the CLI captures the selected
branch tip while holding the exclusive Store lock. The API always requires it:

```cpp
auto applied = store.replay_directory(project, "main", expected_id, source_id,
                                      syrnck::NativeReplayKind::cherry_pick,
                                      "Apply selected changes", {}, operation);
```

Apply recomputes the plan and always verifies both input closures and file hashes.
It rejects unresolved conflicts, empty results, pending checkout/switch recovery and any
existing staging index. The clean-tracked checkout policy applies to all current
files and destination paths; there is no force mode. A new ordinary v1 revision
uses the supplied message, current UTC timestamp and exactly one parent: the
previous target-branch tip. Cherry-pick does not add the source as a merge parent.
Merge input needs the selected mainline; the result still has one parent.

The engine then applies this immutable candidate through the existing recoverable
checkout journal. Working files change before the atomic branch/archive/receipt
publication; the branch remains at the old tip until every required file is
verified. The old tip, its history and file blocks remain reconstructible through
the new parent chain and checkout archive. Symbolic HEAD and the source's branch
or tags do not move. To preserve an unrelated source's complete original history,
keep its own branch, tag or pin; replay imports a delta rather than its ancestry.

The result uses the checkout receipt fields, including `target`, `state` and
`archive`, with `operation`, `replayed_revision`, `previous_revision` and
`mainline` added to the direct command response. A recovered journal reports its
generic checkout identity and recorded target, without these response-only labels.
There is no revision-format or journal-format change.

## Interruption, abort and retention

Before a journal exists, cancellation/failure preserves the old branch and
workspace. Candidate preparation may leave an unreferenced immutable revision.
After journal publication, interruption requires the existing explicit workflow:

```text
syrnck native checkout-status
syrnck native checkout-recover
# Or, before branch publication:
syrnck native checkout-recover --abort
```

Resume finishes the recorded candidate without recomputing the original replay.
Abort restores original working files while retaining the old branch. Once the
branch has published, recovery can only finish cleanup. Opening the Store does
not modify working files, and GC is blocked while a journal is active. Inspect
the resulting head before retrying a completed command: an old `--expect` then
fails instead of creating another commit. Empty replays are rejected rather
than creating an empty commit.

Completed new revisions and their parent history survive destructive GC. An
aborted, unpublished candidate may be collected after its temporary pins are
removed. Checkout archives and receipts have no automatic expiry. Private
prepared files and backups are plaintext even with encrypted object storage;
see [checkout recovery and data limits](NATIVE_CHECKOUT.md).

Tests exercise root and ordinary commit replay, merge-parent selection, all
file-level conflict categories, independent later changes, wrong expected tips,
staging/pending refusal, missing blocks, cancellation, reopen/resume/abort and
reconstruction after GC. Process-kill tests cover both operations and their
rollback through candidate writes, journal transitions, prepared-file publication,
working-file moves and atomic branch/archive publication in plain/encrypted Stores.
