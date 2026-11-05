# Bounded text differences

The independent C++ `compare_text(source_bytes, target_bytes, options, operation)`
returns deterministic line changes without writing objects, references or files.
It compares bytes exactly: whitespace, CRLF/LF and the final newline are significant.
UTF-8 is required. Invalid encodings, NUL and control characters other than tab,
LF and CR immediately before LF are reported as `status: binary`; no text hunks
are emitted. UTF-16 and arbitrary assets are not decoded automatically.

`status: text` has `equal`, source/target byte and line counts, total `additions`
and `deletions`, and ordered `hunks`. Each hunk has source/target start/count and
`lines` with `kind` (`context`, `remove`, `add`), `text` (without LF) and `newline`.
CR in a CRLF line stays in `text`. Starts are one-based, except a zero-count range
uses the number of preceding lines, matching unified diff conventions. Empty
files have zero lines. No trailing empty line is invented after a final LF.

The engine removes a common prefix/suffix and computes a shortest line edit
script with bounded Myers frontiers. Equal alternatives use a deterministic
tie-break. Repeated lines need not align like another diff implementation. This
is a line diff, not word highlighting, rename detection or automatic merging.

Default limits, adjustable through `TextDiffOptions`:

| Budget | Default | Maximum |
| --- | ---: | ---: |
| Context lines | 3 | 100 |
| Bytes per input file | 1 MiB | 4 MiB |
| Lines per input file | 50,000 | 100,000 |
| Work units | 16,777,216 | 268,435,456 |
| Serialized JSON output | 4 MiB | 8 MiB |
| Retained frontier trace | 1,048,576 integers | Fixed |

Work charges a unit for each diagonal/backtracking step and conservatively
charges the line byte length plus one for an equal-length line comparison.
Input scanning, UTF-8 validation, splitting and output construction are also
bounded by their respective byte/line/output limits. Frontier storage is about
4 MiB on supported targets; line views, edit records, JSON, input buffers and
allocator/container overhead are additional. These limits are not a total RSS cap.

An exceeded budget returns `status: limited`, an explicit `reason` (`file_limit`,
`line_limit`, `work_limit`, `trace_limit`, `output_limit`) and no partial hunks or
edit counts. `equal` is null if the input byte limit prevented comparison;
otherwise it describes byte equality, even if a later budget is exceeded.
Binary results also have no line/edit counts. Invalid option values throw.
Cancellation and observer exceptions propagate, rather than becoming a limited
or successful diff. Progress callbacks use the calling thread and must not
reenter the Store.

Tests reconstruct target bytes from hunks, check minimum edit counts against an
independent quadratic LCS oracle, and cover Unicode, mixed line endings, missing
final newlines, repeated lines, each budget and interruption.
