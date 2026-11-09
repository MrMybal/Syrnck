---
id: native-file-history
title: Renames and first-parent file history
description: Exact and edited renames, file lineage and rename-aware merging.
section: Version control
order: 250
parent: native-history
revision: 2
---

# Renames and first-parent file history {#exact-renames-and-first-parent-file-history}

The C++ APIs in `syrnck/history.hpp` provide exact comparison and file history from verified manifest metadata. Default queries do not reconstruct payloads. Explicit similarity options add edited renames and may read small files. Queries never read working files or publish references. Metadata-only comparison does not replace a complete integrity audit.

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
Copies and swaps between existing paths are not inferred. Edited renames and
rename-aware replay require the explicit options below.
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

## Edited renames and similarity {#edited-renames}

```text
syrnck native diff BASE HEAD --rename-similarity 50
syrnck native log-file HEAD Source/New.cpp --rename-similarity 50
syrnck native blame HEAD Source/New.cpp --rename-similarity 50
```

The five-argument `native_diff_renames` overload takes `NativeRenameOptions`
and `OperationOptions`. File-history/blame options expose optional `renames`.
Defaults stay exact-only; `--rename-similarity N` opts into similarity.

Unique whole-file matches are paired first. Identical-content ambiguities are
not rescored. Remaining removed/added paths use common native block identities,
weighted by their byte counts. Verified UTF-8 candidates up to 256 KiB also use
line-delimited spans of at most 64 bytes, recognizing edits within a block.
Larger files use blocks; changed chunking can reduce binary similarity. Small
binary candidates may be read to determine whether they are text. Large
payloads are not reconstructed for scoring.

The score is common bytes divided by the larger file size, taking the greater
of the block and text-span scores. Repetitions count only up to their weight on
each side. An inverted index avoids unconditional all-pairs comparison; highly
repetitive input can still exhaust work limits. Best fractions are compared
exactly before rounding percentages. Only reciprocal unique best matches
meeting N (1..100) are paired. Ties remain explicit ambiguities. This heuristic
does not prove rename intent or preserve ordering: even 100 may accompany
`content_changed: true`. Use exact-only `--renames` when identity is required.

| Option | Default | Accepted range |
| --- | --- | --- |
| `--rename-max-candidates` | 4096 distinct candidate manifests | 1..16384 |
| `--rename-max-work` | 1000000 feature/index visits | 1..16000000 |
| `--rename-max-input-bytes` | 67108864 additional input bytes | 1024..268435456 |

Budgets require `--rename-similarity`. They cover extra decoded manifests and
reconstructed similarity inputs, not all allocations or ordinary snapshot reads.
`rename_detection` exposes usage. History shares a cache/budget over its page;
repeat similarity options with `next_revision`/`next_path`. `--no-follow` is
incompatible. Blame charges these reads against its global input budget too.
Limited diff/history queries fail; limited blame returns empty attribution.

## Rename-aware merges and replay {#rename-merges}

```text
syrnck native merge-plan feature --rename-similarity 50 --text-merge
syrnck native cherry-pick-plan COMMIT --rename-similarity 50 --text-merge
syrnck native revert-plan COMMIT --rename-similarity 50 --text-merge
syrnck native rebase-plan BASE --onto upstream --rename-similarity 50 --text-merge
syrnck native rebase-index-plan --rename-similarity 50 --text-merge
```

Repeat the same options on apply. `NativeMergeOptions`, `NativeReplayOptions`,
`NativeRebaseOptions` and `NativeStageRebaseOptions` have optional `renames`.
A modification under an old name follows a unique rename, including mode changes.
Two renames to the same destination use the same baseline. `--text-merge` composes
compatible text edits; binary contents are selected as complete manifests or
remain conflicts.

Plans expose `renames` with original/current/incoming paths and similarity
evidence. Different destinations (`rename_rename`), rename/deletion
(`rename_delete`/`delete_rename`) and a newly occupied destination (`rename_add`)
need explicit choices. Resolve every listed path with the usual resolution
context; the original path is included so a base choice can restore it. Content choices at an aligned destination use its mapped descriptors.
Case and file/directory guards stay active. Conflict copies preserve competing
contents; they do not resolve structural rename choices or reuse original names.

Rebase shares one cache/budget over the sequence, including virtual text; the
second candidate-generation pass must reproduce the same target. Index rebase
retains staged representations and archives its previous index without changing
working files. Apply verifies complete histories and keeps signature/reference/
checkout guards. A prepared checkout resumes its frozen target after process
termination without inferring the rename again. GC is blocked while checkout is
pending; retained roots still protect versions after recovery.
