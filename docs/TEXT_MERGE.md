# Bounded three-way text merge

The C++ core exposes `merge_text(base, current, incoming, limits, operation)`.
It compares both versions against the same base and composes independent line
edits. Identical changes on both sides are emitted once. The result is conservative:
overlapping changes with different results remain conflicts, including an insertion
at the boundary of a replacement. There is no character-level merge or rename
inference.

## Result

The JSON report contains `version: 1`, input byte and line counts,
`current_changes`, `incoming_changes`, `status`, `reason`, `conflicts` and `content`.
Change counts refer to contiguous edit groups against the base.

| Status | Content | Meaning |
| --- | --- | --- |
| `clean` | Exact merged UTF-8 string, including the empty string | All changes compose |
| `conflict` | `null` | At least one overlapping group differs |
| `binary` | `null` | An input is invalid UTF-8 or contains disallowed controls |
| `limited` | `null` | An input, result, search, trace or output budget was exceeded |

Each conflict contains `base`, `current` and `incoming` ranges with `start` and
`count`. Nonempty ranges use one-based line numbers. An empty range names the
preceding line, including zero at the beginning of a file. Ranges account for
earlier insertions and deletions on each side. A connected set of overlapping
changes produces one conflict. Conflict rows contain coordinates, not marker text.

LF, CRLF, mixed accepted line endings and a missing final LF are preserved byte
for byte. No newline or conflict marker is invented. Binary inputs are rejected
even when all three inputs are equal. A limited result clears all conflict rows,
change counts and content rather than returning a partial composition.

## Budgets and cancellation

The operation uses `TextDiffOptions`: by default 1 MiB per input/result, 50,000
lines per input/result, 16 Mi work units and 4 MiB of serialized output. The hard
ceilings match text diff: 4 MiB, 100,000 lines, 256 Mi work units and 8 MiB.
`context` is validated but does not affect merge decisions.

Both Myers searches and composition share one work budget. The existing bounded
Myers trace is reused; it has a 4 MiB ceiling per search. Line tables, change
groups and the result remain bounded by the file and line limits. These limits
are not a total process-memory promise. The complete serialized merge report,
including JSON escaping of clean content, must fit the output budget.

`reason` identifies the exhausted budget (`file_limit`, `line_limit`,
`work_limit`, `trace_limit`, `output_limit`, `result_file_limit` or
`result_line_limit`). An assembly that would join unterminated lines is refused
with `unterminated_line_join`. Cancellation and observer exceptions propagate;
the function does not write objects or files.

Tests cover independent compositions, side symmetry, overlapping groups and
their coordinates, exact line endings, empty/binary inputs, budgets and
cancellation. The existing text diff oracle also checks the shared edit search.

## Merge one stored path

`store.merge_revision_file(base_id, current_id, incoming_id, path, limits,
operation)` previews the same composition using three explicit native snapshots.
It does not select a graph merge base. The caller supplies that base, which can
come from a native merge/replay plan.

The envelope contains the three revision IDs, nullable `base`, `current` and
`incoming` file summaries (manifest, size, file hash and executable flag),
`ready`, `objects_verified`, `result_executable`, `blockers` and `merge`.
Only a clean result is ready and supplies a result mode. Executable bits merge
independently: an unchanged current mode takes the incoming mode, otherwise the
current mode is retained. Different manifest representations do not change
logical text equality.

All three paths must exist. An absent path produces `file_presence` in `blockers`
and `merge: null`, so deletion is never confused with empty content. All three
sizes are checked before any chunks are read; oversized input produces
`file_limit` and `merge: null`. Both cases have `objects_verified: false`.
Otherwise the complete requested files and their whole-file hashes are verified,
and `merge` contains the core report. Binary, conflict and later budget results
remain non-ready, with the core reason copied into `blockers`. Missing or corrupt
payloads throw rather than becoming a merge result.

Verification covers the requested files, not other paths or revision ancestors.
The operation does not read the working directory, write objects or update
references. The envelope has a separate 16 MiB hard ceiling; the nested core
report obeys `max_output_bytes`. Tests run in clear/encrypted stores with loose,
single-pack and multi-pack layouts, including incomplete stores, corrupt file
hashes, missing payloads, cancellation and physical read-only checks.
