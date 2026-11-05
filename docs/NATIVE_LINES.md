# Native individual text-line selection

Prepare or remove individual added/removed rows inside a text hunk. Existing
[whole-hunk commands](NATIVE_HUNKS.md) remain available. Working files are never
written by either form of selection.

```text
syrnck native add-lines-plan Source/Player.cpp
syrnck native add-lines Source/Player.cpp --expect-plan PLAN_ID --lines 1,3
syrnck native reset-lines-plan Source/Player.cpp
syrnck native reset-lines Source/Player.cpp --expect-plan PLAN_ID --lines 2,4
```

Use the numbers from the corresponding fresh preview, with the same options.
The add and reset plans have different identities and may have different rows.
English is the default interface; French is available with `--lang fr`. JSON
keys, numbers and plan IDs are language independent.

## Numbered change rows

Each preview includes `selection_unit: line`, `change_count` and the ordinary
numbered `diff.hunks`. Each `add` or `remove` row additionally has a global
one-based `change_number`, in diff order across all hunks. Context rows have no
change number. `--lines` selects these change numbers, not working-file line
numbers or positions within a hunk. The list must be nonempty, unique, positive
and in range, with at most 100,000 numbers; input order does not change the bytes.
More than 100,000 change rows blocks the entire preview with `selection_limit`.

For a replacement, old and new rows are independent:

```text
change_number  kind    text
1              remove old A
2              remove old B
3              add    new A
4              add    new B
```

Adding `--lines 2,4` removes `old B` and inserts `new B`, retaining `old A`.
Adding only `--lines 3` keeps both old rows and inserts `new A` after them.
Adding only `--lines 1` deletes `old A` without inserting a replacement.
The original diff order determines insertion positions; selected rows cannot be
reordered or edited. This does not validate syntax or compilation of the result.

For reset, the same forward baseline-to-index numbering identifies changes to
remove. Unselected change rows remain prepared. The result is constructed from
the original baseline plus those retained rows, avoiding shifted-line matching.
Resetting all rows restores baseline content while retaining the index's mode,
other files and staging root. Obtain a fresh preview after every application.

## Exact bytes and selection refusal

Selected text retains its exact UTF-8, LF/CRLF and final-newline bytes. No newline
is inserted automatically. A selection that would append a logical line after
an unterminated retained line is refused before ingestion:

```text
source: old (no final LF)
target: new (with final LF)
```

Selecting only the added row would concatenate `old` and `new`; select both
the removal and addition to replace the line. The equivalent invalid partial
reset is also refused. A ready preview means the comparison is eligible for
selection; some individual combinations may still fail this boundary rule or
the result-size budget. A result can be larger than either input when it keeps
unselected deletions while inserting selected additions.

## Plans, storage and retention

The add preview (`operation: stage-lines`) compares the frozen index/head to the
observed working file. It accepts `--directory` and uses the existing targeted
working reader's ignore, namespace, link, size and observed-change checks.
Application revalidates working path metadata after ingestion and immediately
before reference publication, after the progress observer.

The reset preview (`operation: reset-lines`) compares the index's original
baseline to its frozen bytes. It has no `--directory` argument and never reads
working content. Working paths may be absent, dirty, ignored or replaced by a
directory. Both sides of the stored comparison must contain the file.

Both forms require existing text files, a fresh index when present, and no
pending checkout. New files, complete presence changes, binary/limited diffs,
mode-only changes and archive conflicts follow the whole-hunk restrictions.
An unborn branch can add selected lines against an existing initial index;
its initial file additions require file-level reset.

All commands accept `--branch`, progress/cancellation and the text options
`--context`, `--max-file-bytes`, `--max-lines`, `--max-work`, `--max-output-bytes`.
These keep [text diff's bounds](TEXT_DIFF.md). Row numbering and state metadata
are additional to the diff output budget; the complete report is capped at
16 MiB. Changes to options, numbered diff, branch/index/base, readiness or
observed working content require a new exact `--expect-plan`. A whole-hunk
plan cannot authorize a line application. Ready previews exit 0; blocked
previews exit 1 with JSON. Invalid selections fail without a result.

Publication reuses the verified atomic reference transaction, preserving other
frozen entries and the source index/head executable flag. It publishes the
replacement staging root together with `archive/stage-index/OLD_INDEX_ID`,
when an old index exists. The complete previous preparation and its baseline
remain reconstructible after GC and after committing the new selections.
Cancellation, observer exceptions and failed closure verification preserve the
old references; complete unreachable objects may remain. No callback runs after
reference publication. See [retention and interruption](NATIVE_HUNKS.md#atomic-retention-and-recovery).

C++ callers use `plan_stage_lines(directory, branch, path, limits, operation)`,
`stage_lines(directory, branch, path, expected_plan, numbers, limits, operation)`,
`plan_reset_lines(branch, path, limits, operation)` and
`reset_lines(branch, path, expected_plan, numbers, limits, operation)`.
No logical object or encryption format changes are introduced. Rebuild C++
consumers against the updated header.
