# Native cherry-pick and revert planning

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
are `add_add`, `delete_modify` and `modify_delete`. There is no automatic text
merge, binary overwrite or rename detection. Independent additions are also
checked together for portable `case_collision` and `file_directory_collision`.

The report contains the operation, branch, expected `head`, selected `source`,
`parent`/`mainline`, `ready`, `empty`, `objects_verified`, `result_files`, proposed
`changes`, and `conflicts`. Changed entries include complete manifest/hash/size/mode
summaries. Conflict entries show baseline, current and incoming summaries; path
collision entries identify the other path. Conflicts prevent application of the
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
metadata and hashes working files, but does not verify incoming block payloads.
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
