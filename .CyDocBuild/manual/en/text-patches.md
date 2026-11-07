---
id: text-patches
title: Strict unified text patches
description: Strict unified text patches guide.
section: Version control
order: 570
parent: native-history
revision: 1
---

# Strict unified text patches {#strict-unified-text-patches}

The independent C++ `apply_text_patch(source, patch, path, limits, operation)`
returns `TextPatchResult` with complete result `content` and hunk/addition/deletion
counts. It never writes files, objects or references. Invalid input throws;
no partial result is returned. Inputs remain unchanged.

## Accepted format {#accepted-format}

One existing UTF-8 text file per patch, with exactly matching file labels:

```text
--- a/Source/Player.cpp
+++ b/Source/Player.cpp
@@ -1 +1 @@
-old line
+new line
```

Unprefixed matching labels and Git's trailing TAB label terminator are also
accepted. An optional exact
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
The index-line mode is informational and never changes the source executable flag.

## Exact bytes and bounds {#exact-bytes-and-bounds}

Body rows retain their literal content and LF/CRLF bytes. A standard
`\ No newline at end of file` marker applies to the preceding body row; markers
cannot be orphaned or repeated. Patch body rows themselves must end with LF,
with the marker representing a missing content LF. A truncated final row is an
error. Empty unterminated rows and concatenating content after an unterminated
line are refused. No newline or encoding normalization is performed.

Source, patch and result use the same UTF-8/control-character classification
as [text differences](doc:text-diff). The limits are `TextDiffOptions`:

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


## Prepare an external patch in the native index {#prepare-an-external-patch-in-the-native-index}

C++ callers use `store.plan_stage_patch(branch, path, patch_bytes, limits, operation)`
and `store.stage_patch(branch, path, patch_bytes, expected_plan, limits, operation)`.
The source is the complete frozen file in `staging/BRANCH`, or `heads/BRANCH`
without an index. No working directory argument or working-file read is needed.
Dirty, absent, ignored or directory-replaced working paths do not affect it.
The source file must already exist in the stored snapshot; initial preparation
on an unborn branch is supported when an existing initial index contains it.

The read-only plan reports `operation: stage-patch`, branch/path/head/index/base,
source revision and verified file metadata, raw BLAKE3 `patch_hash`, patch byte
count and result byte/hash/hunk/addition/deletion counts. Inspect the original
patch for its rows; the report does not echo or render the patch. The canonical
preview JSON, before adding `plan_id`, is hashed in the existing chunk domain.
No plan object is written. Options (including validated `context`), patch bytes,
source state and result identity are bound to that exact preview.

Application recomputes and requires the same expected ID before ingestion.
Invalid format, missing/oversized sources, binary input and context/range/budget
failures throw without writing objects or references. Valid previews may still
be blocked by pending checkout, stale staging, an identical result or a
conflicting old-index archive. Plans verify only the selected file's payload;
publication verifies the complete candidate, retained index and branch closures.
Corrupt unrelated data can therefore block publication after a valid preview.

Application starts from the complete frozen index/head file map, changes only
the selected manifest and preserves its executable flag and other preparations.
It ingests default CDC with repository compression/worker settings, then uses
the same atomic publication as [hunk/line selection](doc:native-hunks): head/index/
symbolic HEAD/archive expectations guard the replacement staging root and
`archive/stage-index/OLD_INDEX_ID`. Exact archives can be reused. The previous
index and its baseline remain reconstructible after GC and later commit.

Cancellation, observer failure or failed closure checks before publication keep
the old references; complete unreachable objects may remain. No workspace
journal is needed. No observer runs after reference publication. The complete
preview/application report is capped at 16 MiB, independently of patch input.
The caller keeps the borrowed patch bytes stable for the duration of each call.

## CLI workflow {#cli-workflow}

```text
git -c core.quotepath=false diff --no-ext-diff --no-textconv --output=change.patch -- Source/Player.cpp
syrnck native add-patch-plan Source/Player.cpp --patch change.patch
syrnck native add-patch Source/Player.cpp --patch change.patch --expect-plan PLAN_ID
syrnck native diff-text --staged Source/Player.cpp --unified
syrnck native commit --staged -m "External text changes"
```

Git writes patch bytes directly with `--output`, avoiding shell text encoding
conversion. Use a single existing file and the intended source state. Git's
index hashes do not authorize or identify Syrnck objects. Modified-file patches
from `native diff-text SOURCE TARGET PATH --unified` are also accepted. Git
fixtures cover Unicode/spaces, CRLF/LF, missing final LF, empty existing files,
clearing file contents and multiple hunks.

Both commands require `--patch FILE`, accept `--branch NAME` and the text limits,
and support `--progress` / `--progress-json` and cancellation. They accept no
`--directory`: the patch is applied to stored bytes. The patch file is read once,
within the input byte budget, before planning/application. Its captured bytes
are bound by the plan hash; a changed file on a later invocation needs a new
preview. Review the original patch and source/result hashes before application.

The default UI is English, with French via `--lang fr`; JSON is identical in
both languages. A ready preview exits 0; a blocked preview exits 1 with JSON.
Invalid patches/options or failed applications exit nonzero without a result.
Successful application returns JSON with `state: applied`, previous/new index,
result manifest and file hash, keeping the original preview ID. Progress stays
on stderr. Other prepared files and modes survive the subsequent staged commit.
