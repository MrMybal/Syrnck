# Strict unified text patches

The independent C++ `apply_text_patch(source, patch, path, limits, operation)`
returns `TextPatchResult` with complete result `content` and hunk/addition/deletion
counts. It never writes files, objects or references. Invalid input throws;
no partial result is returned. Inputs remain unchanged.

## Accepted format

One existing UTF-8 text file per patch, with exactly matching file labels:

```diff
--- a/Source/Player.cpp
+++ b/Source/Player.cpp
@@ -1 +1 @@
-old line
+new line
```

Unprefixed matching labels are also accepted. An optional exact
`diff --git a/PATH b/PATH` preamble and `index OLD..NEW [100644|100755]` line are
accepted. Index hashes are format hints, not verified Git identities; source
bytes and hunk context determine applicability. Hunk counts may be omitted
when one. Optional section text after `@@` is accepted. Positions and counts
must agree on both source and result, including zero-count insertion ranges.
Every context/removal row must match the source byte-for-byte at that position.
There is no offset search, fuzz, whitespace correction or reverse guessing.

File labels are literal UTF-8, with LF or CRLF metadata terminators. Quoted/
escaped labels, timestamp suffixes, renames, mode changes, file presence changes,
`/dev/null`, binary patches, combined/word diffs, multi-file input and mail/commit
wrappers are outside this subset. A second file or trailing unsupported data
is refused. An empty patch or one containing no added/removed rows is refused.
The selected portable path is validated but is never opened by this function.

## Exact bytes and bounds

Body rows retain their literal content and LF/CRLF bytes. A standard
`\ No newline at end of file` marker applies to the preceding body row; markers
cannot be orphaned or repeated. Patch body rows themselves must end with LF,
with the marker representing a missing content LF. A truncated final row is an
error. Empty unterminated rows and concatenating content after an unterminated
line are refused. No newline or encoding normalization is performed.

Source, patch and result use the same UTF-8/control-character classification
as [text differences](TEXT_DIFF.md). The limits are `TextDiffOptions`:

- `max_file_bytes` bounds source and result independently (default 1 MiB, cap 4 MiB).
- `max_output_bytes` bounds patch input (default 4 MiB, cap 8 MiB).
- `max_lines` bounds each source/result (default 50,000, cap 100,000).
- `max_work` charges each consumed patch row's bytes plus one, matched source
  bytes plus one, and copied unchanged source bytes plus one. Validation scans
  are additionally bounded by the byte limits.
- Hunk positions/counts and the number of hunks are capped at 100,000.
- `context` remains validated but does not change an externally supplied patch.

Memory holds bounded source line views and result bytes; it does not run an
edit-distance search or retain a diff frontier. Cancellation is checked during
validation, traversal and output. Synchronous planning observers can abort by
throwing; neither failure returns a successful or partial result.

Tests cover fixed/mixed newline cases, Unicode, missing final newlines, empty
existing contents, zero-count ranges, omitted counts, Git preambles, 1,000
deterministic random diff/patch round trips, malformed/truncated/multi-file
patches, path/range/context mismatches, all budgets and interruption.
