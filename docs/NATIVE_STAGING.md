# Native staging and selected commits

The standalone engine can prepare selected files, inspect the frozen snapshot,
remove selections and publish a commit without reading the working files again.
This operates on native branches; Git retains its own ordinary index.

```text
syrnck native add Source Content/Maps --branch main --progress
syrnck native staged --branch main
syrnck native reset Source --branch main
syrnck native commit --staged -m "Update the selected maps" --branch main
```

`native add .` selects the entire candidate workspace. `native reset` without
paths discards the branch's staging index. None of these commands changes working
files. The CLI defaults to the [current native branch](NATIVE_BRANCHES.md),
implicitly `main` in legacy Stores; `--branch` overrides it without selecting it.

## Selection and frozen contents

Paths select an exact file or all entries below a directory prefix. They are
relative to `--directory` (default `.`), rather than relative to a Git root.
Forward slashes are portable; the CLI also normalizes platform path separators,
`./` and a trailing slash. The C++ API expects validated repository paths or `.`.
Wildcards, absolute paths and paths outside the root are rejected. Use `--`
before a path starting with `-`.

Each add updates the existing index, initially copied from the branch snapshot.
Unselected entries retain their previously prepared versions. Selected working
files are ingested immediately. A selected tracked/prepared path absent from the
candidate workspace is removed from the index, including tracked files excluded
by current ignore rules. A path that matches neither a selected working file nor
an indexed/baseline file is an error; an untracked empty directory has no entry.
Overlapping selections and repeated paths are allowed. `dir` does not select
`dir2`. Select their common ancestor to stage a file/directory or case change
that would otherwise leave conflicting entries in the snapshot.

The scanner and exclusions are shared with native commits/status: nested
`.syrnckignore`, portable path/case collision checks and regular-file restrictions
apply. The Store must be inside the workspace. The scanner currently visits the
whole candidate workspace and ingests only selected files. An invalid unselected
path can therefore still reject the scan. There is no watcher or scan cache.
Scanning and flat revision metadata retain the existing 16 MiB limits.

Add prints the immutable staging revision ID. It describes a complete snapshot,
including unchanged files, and can be inspected/exported with the existing
revision tools. Repeating an add with the same branch base and file entries
produces the same ID. Later working edits do not change prepared bytes. Adding
the path again explicitly replaces its prepared version.

## Inspect, reset and commit

`native staged` reads only revision/manifest metadata and emits English JSON:

| Field | Meaning |
| --- | --- |
| `branch` | Explicit native branch name |
| `base` | Branch revision recorded when this index started, or null |
| `head` | Current branch revision, or null |
| `snapshot` | Staging revision ID, or null if no index exists |
| `stale` | Current branch revision differs from the recorded base |
| `clean` | Indexed file entries equal the recorded baseline |
| `staged_files` | Total files in the candidate snapshot, including unchanged files |
| `counts`, `changes` | Added/removed/modified entries and unchanged count |

Change rows follow [native revision comparisons](NATIVE_COMPARISON.md), including
content, executable and manifest changes. Representation-only changes can be
staged even when file bytes match. A missing index reports a clean comparison
against the current branch; an unborn branch uses an empty baseline. A stale
index remains inspectable against its original base. Inspection does not audit
payload availability; publication and GC verify full retained closures.

`native status` continues to compare the working directory with the branch (or
an explicit revision). Use `native status --revision STAGING_ID` to compare
working bytes with the prepared snapshot. `native status --combined` returns
both comparisons together, retaining stale-index information. See
[combined native workspace status](NATIVE_WORKSPACE.md).

`native reset PATH...` restores selected index entries from the recorded base,
or removes newly added entries. It does not read or restore working files.
It leaves an explicit index, even when all entries match the baseline.
`native reset` clears the entire index and is idempotent when none exists.

`native commit --staged -m MESSAGE` publishes the exact index, sets the recorded
base as its parent and consumes the index. An unborn branch produces a root
commit. It requires an index with changed file entries. It accepts ordinary
progress/cancellation options, but rejects directory/ingestion options because
it does not ingest working files. The ordinary `native commit` still snapshots
the whole workspace and now refuses when that branch has an index; commit the
selection or reset it explicitly first.

If another operation moves/removes the branch, add, selective reset and staged
commit refuse the stale index. Inspect/export it if needed, then explicitly
clear it with `native reset`, or use `native rebase-index-plan` and
`native rebase-index` to replay its frozen changes onto an existing branch head.
The latter archives the old index and preserves working files; conflicts block
publication. See [staging rebase](NATIVE_STAGE_REBASE.md). Partial hunks and text
patches remain future work.

## Retention and publication

Each branch has one `staging/BRANCH` reference to an ordinary v1 revision object.
Its fixed timestamp, branch-bound internal message and optional baseline parent
identify the index. It contains a full file map, not a chain of staging edits.
`heads/BRANCH` remains unchanged until commit. These internal revision objects
are not added as parents of user commits.

The staging root protects all prepared manifests/chunks and the baseline history
against GC, including while the branch itself is absent. Plain/encrypted and
loose/single-pack/multi-pack Stores use the same logical mechanism. The ordinary
format and encryption-at-rest behavior are unchanged. Generic reference tools
can remove these protections; do not drop `staging/` roots for active indexes.

Add and selective reset validate complete replacement closures before one
reference transaction. Staged commit verifies all required objects, then moves
the branch and removes the index in a single atomic `refs.json` replacement.
Expected branch/index values are checked together. No callback or cancellation
check runs after this publication. Full reset atomically removes only the index.
The exclusive Store lock remains held throughout each operation.

Cancellation, source changes, invalid paths, missing dependencies and callback
failure before publication preserve the previously published branch/index.
Ingestion or revision preparation may leave unreferenced immutable objects;
retry can reuse them, or a later explicit GC can collect them. Workspace files
are never rolled back because staging never writes them. Scanning is not an
atomic filesystem snapshot: individual files are checked around ingestion, and
concurrent edits can require retrying.

## C++ API and validation

`native commit --amend --staged -m MESSAGE` can replace the existing tip using
the same prepared snapshot, including a clean index for a message correction.
It preserves the old tip's parents and atomically archives the old revision while
consuming the index. See [native amendment and retention](NATIVE_AMEND.md).

```cpp
auto snapshot = store.stage_directory(project, "main", {"Source", "Content/Maps"});
auto report = store.staged_status("main", operation);
auto remaining = store.reset_stage("main", {"Source"}, operation);
auto commit = store.commit_staged("main", "Selected changes", operation);
store.reset_stage("main");
```

An overload of `stage_directory` accepts `IngestOptions`; the default uses
repository ingestion settings. The other operations accept `OperationOptions`.
Callbacks run synchronously on the caller and must not reenter the Store.
Add reports scan/ingestion progress; publication verifies closures and ends with
`publishing` at `refs`. C ABI and Go native-revision bindings remain future work.

Regression coverage includes frozen selected bytes, deletion/ignore selection,
unborn and independent branches, stale indexes, path transitions, selective/full
reset, reopen/GC/packing, cancellation at scan/ingest/verify/publication, source
mutation and callback exceptions. Process-kill tests exercise staging writes,
atomic branch/index publication and reset with the production recovery reader.

## Inspect prepared text

Use `native diff-text --staged PATH [--branch NAME] [--unified]` for a verified, bounded line diff against the frozen index’s original base. Stale indexes keep their baseline and later working-file changes are not read. See [text diff contracts and limits](TEXT_DIFF.md). Hunk selection remains future work.
