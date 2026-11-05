# Native partial text staging

Select complete numbered text hunks from an existing file without changing the
working copy. This complements ordinary file/directory staging.

```text
syrnck native add-hunks-plan Source/Player.cpp
syrnck native add-hunks Source/Player.cpp --expect-plan PLAN_ID --hunks 1,3
syrnck native staged
syrnck native diff-text --staged Source/Player.cpp --unified
syrnck native commit --staged -m "Selected changes"
```

Both commands accept `--branch NAME`, `--directory PATH`, progress/cancellation,
and the same `--context`, `--max-file-bytes`, `--max-lines`, `--max-work` and
`--max-output-bytes` options as [text diff](TEXT_DIFF.md). English is the default
interface; French is also available. JSON keys, plan IDs and hunk numbers are
language independent. Application returns JSON, rather than the quiet ID output
of ordinary `native add`.
The output budget bounds text-diff generation; numbering and context metadata
are additional. The complete preview/application report is capped at 16 MiB.

## Review and bind the selection

The read-only plan compares the frozen index (or branch head without an index)
to the observed working file. Its `diff.hunks` have one-based `number` fields.
Context controls grouping; adjacent edits in one hunk are selected together.
Numbers belong only to that exact plan. This does not select arbitrary lines,
edit patch text or present an interactive prompt.

The report includes the working diff, `operation: stage-hunks`, branch/head/index/
base state, optional old-index `archive`, `text_options`, `ready`, `blockers` and
`plan_id`. The ID is the existing chunk-domain BLAKE3 ID of the canonical preview
JSON before adding `plan_id`; no plan object is stored. It binds the directory,
path, branch state, source manifest, observed working hash, available mode,
numbered hunks, limits and readiness. Different interface languages do not change
it. After applying hunks, the returned report retains the original preview ID,
while result fields describe publication; its entire JSON is not that preview's
hash input anymore.

Application recomputes the preview and requires the exact expected ID. Changes
to working content, branch/index state or options require a fresh preview. Pass
the same nondefault options on both commands. Selected numbers must be unique,
positive, in range, and nonempty (at most 100,000). Input order is irrelevant to
the selected bytes. After successful application, preview again before choosing
remaining changes: their numbers can change.

Ready previews exit 0. Blocked previews exit 1 with JSON; ordinary errors exit
nonzero without a result. Blockers include pending checkout, stale index,
non-text/limited diff, no text changes, file-presence changes and conflicting
old-index archives. Preview verification covers this file, not unrelated stored
payloads or full history. The complete replacement and retained closures are
verified during publication; missing unrelated data can still block application.

## Scope and exact bytes

Both source and working file must exist as regular files. Empty existing files,
CRLF/LF, Unicode text and missing final newlines are supported. Binary files,
new/untracked files and complete file deletions require ordinary `native add`.
An unborn branch is supported when an existing frozen initial index already
contains the selected file. Stale indexes must be explicitly rebased or reset.

The candidate starts from the complete source index/head file map. It replaces
only the selected path's manifest, retaining all other frozen files and the
source executable flag. Mode changes and alternate chunking representations
are not selected as text. Individual hunks are copied byte-exactly; unselected
regions remain source bytes. Selecting all hunks reproduces the observed working
content, within the configured file budget. A subset can be larger than either
input when it keeps unselected deletions and includes selected additions; it is
refused before ingestion if it exceeds that budget.

The targeted workspace reader enforces ignore rules, exact path spelling,
namespace/link/storage guards, bounded reads and observed-change detection.
See [working-file guarantees](TEXT_DIFF.md#compare-a-working-file). No workspace
files, branch heads or symbolic HEAD are changed by partial staging.

## Atomic retention and recovery

The selected bytes are ingested using default CDC and repository compression/
worker settings. The new ordinary v1 staging revision has the branch-bound
marker, timestamp zero and original head parent (or no parent initially).
No logical object format or encryption envelope changes are introduced.

One reference transaction guards the branch head, current index, symbolic HEAD
and optional archive. It publishes the new `staging/BRANCH` and, when an index
already existed, `archive/stage-index/OLD_INDEX_ID` retaining the complete former
prepared state and its baseline. Exact existing archives can be reused; a
conflicting target is refused. Inspect them through:

```text
syrnck native archives --kind stage-index --verify
syrnck native archive-show archive/stage-index/OLD_INDEX_ID --verify
syrnck native export OLD_INDEX_ID NEW_DIRECTORY
```

Working path metadata is checked again after ingestion and immediately before
reference publication, after progress observers. Cancellation, observer failure,
detected working changes and failed closure checks preserve the old references.
Unreferenced complete objects can remain and are reusable or collectible. Once
the atomic reference write starts, it may complete; no user callback runs after
publication. After interruption, inspect the index/archive and obtain a fresh
plan. Reusing the old successful plan does not silently apply it a second time.

This is not filesystem snapshot isolation or a guarantee against a hostile
writer restoring timestamps between checks. No workspace journal is needed
because this operation never writes working files. Archives remain retained
through GC until explicitly removed through generic reference administration.

C++ callers use `plan_stage_hunks(directory, branch, path, limits, operation)` and
`stage_hunks(directory, branch, path, expected_plan_id, numbers, limits, operation)`.
Tests cover every subset of three hunks, frozen neighboring files, newlines,
mode preservation, initial/stale indexes, expected-plan drift, cancellation,
observers, archive collisions, missing payloads, result budgets, GC and commit.
