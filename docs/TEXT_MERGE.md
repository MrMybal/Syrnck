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

## CLI and reviewed conflict resolution

```text
syrnck native merge-text BASE CURRENT INCOMING Source/Player.cpp
syrnck native merge-text BASE HEAD feature Source/Player.cpp --text
```

Selectors use the native HEAD/branch/tag rules. The default output is the JSON
envelope; exit status is zero only when `ready` is true. A blocked JSON preview
exits 1 and still writes its complete report to stdout. `--text` writes only
clean merged bytes, without a trailing newline added by the CLI. On conflicts,
binary input, absence or budget limits it fails with no content on stdout.
Diagnostics and help use English by default, or French with `--lang fr`.
JSON fields and merged bytes are language independent.

`--max-file-bytes`, `--max-lines`, `--max-work` and `--max-output-bytes` override
the normal text budgets. `--progress` and `--progress-json` report on stderr;
the operation supports the existing cooperative CLI cancellation.

This command is an explicit preview. Native [merge](NATIVE_MERGE.md#opt-in-text-composition)
and [cherry-pick/revert](NATIVE_REPLAY.md#optional-bounded-text-composition) can also
enable bounded automatic text composition. Rebase still uses its existing
file-level decisions. A reviewed clean text result can be applied through a
custom file resolution:

1. Run the native operation's plan and retain its exact `resolution_context`.
   For a merge, pass its `base`, `head` and `source` IDs to `merge-text`.
2. Inspect the JSON report, including `result_executable`. Save clean `--text`
   output to a temporary file outside the tracked workspace, using a tool that
   preserves stdout bytes. Older shells can transcode redirected native output.
3. Import that file with `syrnck put FILE`. If GC can run before application,
   retain its manifest with a pin, as described in
   [native conflict resolutions](NATIVE_RESOLUTIONS.md#choices).
4. Add a `file` choice using that manifest and the reviewed executable mode to
   the original resolution context. Preview again with `--resolutions` and
   `--verify`, then apply with the same exact head and resolution document.

The normal publication checks, recoverable checkout journal and archive roots
then protect the resulting commit and both parents. Other unresolved conflicts
still block publication. The text preview does not bind or publish a branch
transaction by itself. Root/add/delete cases and virtual multi-step rebase text
composition remain future integration work.

CLI tests compare selected clean outputs byte for byte with `git merge-file`,
exercise Unicode/spaced paths, selectors, bilingual output and failure modes,
and apply a reviewed result through a real merge followed by GC and export.

## Virtual file preparation

The internal native text batch prepares candidate manifest IDs in memory using
the production default CDC boundaries and v1 manifest encoding. It retains clean
generated bytes until application. It writes neither chunks nor manifests during
planning. Materialization uses the normal configured ingestion pipeline and
requires the resulting manifest ID to match the predicted ID before a revision
can use it. Existing representations are reused when the merged logical content
already matches one of the inputs.

A batch attempts at most 64 content conflicts and charges at most 16 MiB for the
sum of verified input bytes and clean result bytes. These aggregate ceilings are
in addition to each file's text limits. Files outside a budget remain unresolved
with `file_count_limit` or `total_bytes_limit`; no text result is guessed. Reports
contain status, ranges and target metadata, never generated plaintext.

Tests compare prediction with actual ingestion from empty content through 4 MiB,
including content with multiple CDC blocks, and exercise deduplication, aggregate
limits, all six storage variants and physical read-only planning.
