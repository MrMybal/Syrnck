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

## Compare a native file between revisions

```text
syrnck native diff-text SOURCE TARGET Source/Player.cpp
syrnck native diff-text HEAD release Source/Player.cpp --context 5 --unified
```

Selectors follow the usual native HEAD/branch/tag rules. The C++ equivalent is
`store.diff_revision_file(source_id, target_id, path, limits, operation)`; either
optional revision can be null to represent an empty snapshot. The path must be
an exact safe repository path, present on at least one side. Absence represents
an added/removed file, independently of whether its content is empty.

The JSON envelope contains `path`, `source_revision`, `target_revision`, nullable
`source`/`target` file summaries (manifest, size, file hash and executable flag),
`equal`, `content_changed`, `executable_changed`, `manifest_changed`,
`objects_verified`, and the nested `diff` result described above. Envelope
`equal` compares presence, logical content and executable mode. Nested
`diff.equal` compares bytes only: an empty-file addition has envelope `equal:
false` and nested `equal: true`. Different chunking can change a manifest while
preserving logical equality.

Both file sizes are checked before reading either file's chunks. A `file_limit`
report is metadata-only and has `objects_verified: false`. Otherwise, both
complete files are reconstructed and their whole-file hashes are verified,
including for binary/equal files or later line/work/output limits. Missing or
corrupt payloads fail the operation rather than yielding a patch. Only this
path's payloads are read; unrelated files and revision parents are not traversed.
The wrapper report is additionally capped at 16 MiB, including metadata/path.

Default output is canonical JSON independent of the interface language. A
successful JSON inspection exits 0, including explicit binary/limited reports.
`--unified` renders text hunks with `a/` / `b/` paths, `/dev/null` for a missing
side and exact missing-final-newline markers. It fails before stdout output for
binary/limited input. An empty hunk list produces empty unified output.
Mode-only changes, alternate manifests and empty-file creation/removal require
the JSON metadata; unified output represents text content only. It is not a
complete native revision patch or a native patch-application command.

`--context 0..100`, `--progress` and `--progress-json` are supported. All modes
accept `--max-file-bytes`, `--max-lines`, `--max-work` and `--max-output-bytes`,
using the defaults and hard maxima in the table above. Options apply to this
invocation only; an exceeded runtime budget remains an explicit limited result.
Invalid limits fail before inspection. Owned errors/help are available
in English and French. Tests apply emitted textual patches with Git and compare
the resulting bytes, including Unicode/space-containing paths and CRLF.

## Inspect frozen staged changes

```text
syrnck native diff-text --staged Source/Player.cpp
syrnck native diff-text --staged Source/Player.cpp --branch feature --unified
```

`store.diff_staged_file(branch, path, limits, operation)` compares that path in
the staging index to its original baseline. Without an index, it compares the
head against itself. An initially unborn index has an empty source snapshot.
Stale indexes keep their original base: the engine never substitutes the current
head or reads the working directory. Later unsaved/unprepared changes do not
affect the result. Prepared deletions use a missing target.

The envelope adds `comparison: staged`, `branch`, nullable `base`, `head` and
`index`, and `stale`. The index's internal branch marker, timestamp and parent
shape are validated before comparison, using the ordinary staging reader.
Branch selection defaults to native HEAD in the CLI; `--branch` overrides it.
Options, integrity checks, JSON/unified behavior and cancellation follow the
revision comparison contract above. The command does not consume/rebase the
index, alter references or read working files. A path absent from both frozen
snapshots is an error, even if a working file now exists there.

Staging integration tests cover unborn/fresh/stale state, prepared deletion,
branch overrides, later dirty files, binary/size limits and immutable Store
contents. C++ coverage uses plaintext/encrypted stores and loose/single-pack/
multi-pack layouts. Line/hunk selection and patch application remain future
work; this milestone is read-only inspection.

## Compare a working file

```text
syrnck native diff-text --working Source/Player.cpp --unified
syrnck native diff-text --working Source/Player.cpp --branch feature
syrnck native diff-text --working Source/Player.cpp --revision HEAD --directory .
```

The default source is the selected branch's frozen index, or its head when no
index exists. `--branch` overrides branch selection. A stale index remains the
source, with its original base and current head reported. `--revision SELECTOR`
instead chooses an explicit committed source; it cannot be combined with
`--branch`. `--working` and `--staged` are mutually exclusive. An unborn branch
without an index compares against an empty snapshot. Working deletions and
untracked/empty additions are explicit in JSON.

C++ entry points:

```cpp
auto explicit_base = store.diff_working_file(project, revision_id, "Source/Player.cpp", limits, operation);
auto unstaged = store.diff_unstaged_file(project, "feature", "Source/Player.cpp", limits, operation);
```

Only ancestor directories are enumerated to resolve the exact path, detect
portable case collisions and apply the existing nested ignore rules. Unrelated
subdirectories and their file contents are not traversed. Ignored paths are
errors, including tracked paths and missing ignored names; they are never
silently reported as deletions. Links/reparse points, case aliases, directory/
file obstructions and the protected Store are refused. The Store must be below
the selected workspace. The CLI also checks native HEAD's workspace identity.
Paths use the filesystem's actual UTF-8 spelling, including Unicode normalization.

Inspection has a 128-component depth bound and a 16 MiB budget for enumerated
names and ignore-file bytes per pass. Bounded ignore parsing also limits rules
to 16,384 entries of at most 4,096 bytes. Glob semantics match native staging;
the limits apply only to this targeted inspector. Ordinary snapshot scanning
keeps its existing policy.

Both source/working sizes are checked before payload reads. Within budget, the
complete source is reconstructed and verified, and the working file is read
through a 64 KiB buffer with growth/shrink checks. Namespace, size, timestamp
and available mode are rechecked around the read. Detected changes fail with
a retry diagnostic. This is an observed working-file read, not a filesystem
snapshot or protection against a hostile writer that restores timestamps or
swaps paths between checks.

The envelope has `comparison: working`, canonical `directory`, `source_revision`
and a null `target_revision`. Working `target.manifest` is null; `file_hash` is
computed only when payloads are read. `workspace_verified` records that the
working read/missing-path checks completed; `objects_verified` records source
payload verification. Both are false on a file-size limit. Size/presence can
prove `content_changed`; otherwise `content_changed` and envelope `equal` stay
null when size limits prevented a content comparison. No equality is invented.
`manifest_changed` is null because a working file has no stored representation.

On Windows, `target.executable` is null, `executable_comparable` is false, and
mode does not affect envelope equality. On Unix, executable mode is compared.
Branch-based results additionally include `branch`, `head`, `base`, `index` and
`stale`. JSON/unified output, cancellation and binary classification follow the
earlier contracts. The command imports no objects and changes no working files,
references or staging state.
