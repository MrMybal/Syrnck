---
id: native-blame
title: Native line attribution
description: Verified blame through merge parents and optional edited renames.
section: Version control
order: 251
parent: native-history
revision: 3
---

## Attribute lines {#usage}

`native blame REVISION PATH` attributes each line of a committed UTF-8 text file
to the revision and path that introduced it. It reconstructs and verifies stored
bytes without reading the workspace, modifying references or importing objects.
The C++ entry point is `native_blame` in `syrnck/history.hpp`.

```text
syrnck native blame HEAD Source/Module.cpp
syrnck --lang fr native blame tags/release Source/Module.cpp --progress-json
syrnck native blame HEAD Source/Module.cpp --first-parent --no-follow
```

The JSON `lines` array keeps final line numbers, origin `revision`, origin `path`,
one-based `source_line` and exact `content`, including LF/CRLF or a missing final
newline. Empty files have zero lines. `commits` indexes origin revision timestamps
and bounded message previews. V2 entries also expose `author`, `committer` and
the public `signing_key` (null when unsigned). Legacy v1 has no such fields;
this command does not invent them. See [identity and trust](doc:native-authorship).
JSON keys and identifiers remain English.

## Merges and renames {#lineage}

By default every ordered parent can supply unchanged lines. The first matching
parent takes precedence, then the remaining lines are compared with later
parents. Only lines unmatched in every parent originate at the merge itself.
`--first-parent` restricts attribution to the first-parent chain. Matching uses
the bounded Myers line-edit engine; repeated identical lines can admit several
valid alignments. No whitespace normalization is applied.

Unique exact-content renames are followed in either chunk representation, using
the complete-file hash and size. Executable-mode changes do not create new text
origins. Multiple possible removed sources or added destinations stop following;
copies are not inferred. Edited renames require the explicit option below. `--no-follow` treats a newly named
file as an addition. A binary parent cannot supply textual line origins.

## Budgets and errors {#limits}

The default traversal permits 1000 distinct revisions (1..10000), 64 MiB aggregate
decoded revision/manifest metadata and reconstructed file bytes (1..256 MiB), and
the [text engine budgets](doc:text-diff). These input/work budgets do not bound
every allocation in the process. Reconstructed content is cached per manifest
only for this query. Existing retained roots still protect the inspected history.

```text
syrnck native blame HEAD Source/Module.cpp --max-revisions 2000 --max-input-bytes 134217728
syrnck native blame HEAD Source/Module.cpp --max-file-bytes 2097152 --max-lines 60000 --max-work 33554432 --max-output-bytes 8388608
```

`status: complete` exits 0. Binary input returns `status: binary`, reason `non_text`,
and exit 1. An exhausted revision/input/file/line/work/trace/output budget returns
`status: limited`, a reason and exit 1. Both non-complete reports contain empty
`lines` and `commits`; partial attribution is never presented as complete.
Missing paths, corrupt/missing objects, invalid arguments and cancellation fail
the command with no partial report. Progress and diagnostics use the selected
interface language.

## Follow an edited rename {#edited-renames}

`--rename-similarity 50` or `NativeBlameOptions.renames` enables
[bounded similarity](doc:native-file-history#edited-renames). Unchanged lines
keep their earlier revision/path; edited lines originate at the later revision.
Ambiguities stay at the new-name boundary. Similarity reads count in the global
input budget. `rename_limit`/`input_limit` returns `limited` with empty `lines`/`commits`.
