---
id: native-file-history
title: Exact renames and first-parent file history
description: Exact renames and first-parent file history guide.
section: Version control
order: 250
parent: native-history
revision: 1
---

# Exact renames and first-parent file history {#exact-renames-and-first-parent-file-history}

The C++ APIs in `syrnck/history.hpp` add metadata-only exact rename comparison
and file-history inspection. Neither API reads working files, changes references
nor reconstructs asset payloads. Their content comparisons use verified
manifest metadata; they do not replace a complete file-integrity audit.

```text
syrnck native diff SOURCE TARGET --renames
syrnck native log-file HEAD Content/Map.uasset
syrnck native log-file HEAD Content/Map.uasset --limit 25 --max-revisions 1000
syrnck native log-file HEAD Content/Map.uasset --no-follow
```

## Exact rename comparison {#exact-rename-comparison}

`native diff --renames` first compares snapshots, then pairs a removed path
with an added path when their complete-file BLAKE3 hash and size match exactly
and the pairing is unique in both directions. It works across directories,
case/Unicode spelling changes and different chunking/compression representations.
Executable-mode changes are retained separately in the renamed row.

Renamed rows have `status: renamed`, `path` (destination), `old_path`,
`similarity: 100`, source/target metadata and distinct content/mode/manifest
change flags. Counts add `renamed` while subtracting matched additions/removals.
The ordinary diff command keeps its existing addition/removal behavior.

If identical removed/added contents have several possible pairings, the result
keeps the additions/removals, sets `rename_ambiguous` on the affected rows and
reports ambiguity counts. Existing unchanged copies do not become renames.
Edited renames, similarity heuristics, copy detection, swaps between existing
paths and rename-aware merge/rebase conflict resolution are not inferred.
Report serialization is bounded at 16 MiB.

## Follow a file lineage {#follow-a-file-lineage}

`native log-file REVISION PATH` walks the first-parent history and reports
changes affecting that file. It follows unique exact renames backwards, skips
unrelated/empty commits and includes content, mode, representation and deletion
events. A path deleted or renamed away at the starting revision can still show
its prior history. Each entry carries the revision, path at that revision,
bounded message preview, timestamp, parents and the snapshot change.

The first addition ends the lineage. An ambiguous rename origin also ends it,
with `stop_reason: ambiguous_rename`, rather than selecting an arbitrary source.
`--no-follow` ends at additions under the selected path, including renamed-in
files. Merge commits are compared with their first parent; other parents are
reported as metadata but not traversed. This is not all-parent blame or a
general history-search engine.

Pages default to 100 changes (1..1000). The traversal independently defaults to
1000 inspected revisions (1..10000), so an unchanged file does not require an
unbounded walk. An incomplete page returns `next_revision` and `next_path`:

```text
syrnck native log-file NEXT_REVISION NEXT_PATH --limit 25
```

Together those fields form an explicit continuation cursor, including a path
changed by a rename. `complete` and `stop_reason` distinguish `added`,
`ambiguous_rename`, `root`, `page_limit` and `traversal_limit`. Read-only queries
do not pin their versions; normal retained roots must still protect them from
an intervening administrative retirement/GC.
