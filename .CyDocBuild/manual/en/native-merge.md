---
id: native-merge
title: Native merge
description: Native merge guide.
section: Version control
order: 300
parent: native-history
revision: 3
---

# Native merge {#native-merge}

`native merge-plan` computes the topology and complete proposed file snapshot
without changing objects, references or working files. It shares file-level
three-way decisions with [native replay](doc:native-replay).

```text
syrnck native merge-plan feature --verify
syrnck native merge-plan feature --base BASE_ID
syrnck native merge-plan imported --allow-unrelated-histories
```

The default target is the selected native branch. `--branch` and `--directory`
override the operation's branch and workspace. The CLI accepts native revision
selectors; the C++ API takes exact source/base IDs. `--max-revisions` and
`--max-links` have the same bounds/defaults as [ancestry queries](doc:native-history).
The complete input graph must be available; there is no shallow fallback.

## Topology {#topology}

| Mode | Meaning | Proposed snapshot |
| --- | --- | --- |
| `up_to_date` | Source is already an ancestor of the target, including equality | Exact current branch snapshot |
| `fast_forward` | Current target is an ancestor of source | Exact source snapshot, including its manifest representation |
| `merge` | Neither revision contains the other | Three-way file merge against the selected base |

A unique best common ancestor is selected automatically. Multiple best bases
produce `multiple_merge_bases`; `--base` must select one of the reported best
ancestors. An older common ancestor is rejected. This explicit choice does not
synthesize Git's recursive virtual merge base. No common ancestor produces
`unrelated_histories` unless `--allow-unrelated-histories` explicitly selects the
empty tree as baseline. An explicit base cannot be supplied for unrelated roots.

The file merger combines independent edits and content/mode changes on the same
file. Different concurrent contents, add/add and delete/modify cases remain
conflicts. The combined snapshot is checked for case and file/directory prefix
collisions. Content equality uses file hash, size and mode; rechunking alone is
not an edit. Fast-forward nevertheless adopts the exact source revision.

When snapshot conflicts are absent, the existing clean-tracked checkout preflight
checks working files, ignored/untracked obstructions, protected paths, links,
staging and active recovery. A topology-only divergent merge can be ready even
with no changed file bytes. Up-to-date plans also use this conservative preflight.
Explicit file choices and custom manifests can resolve conflicts with
`--resolutions JSON_FILE`; see [native resolutions](doc:native-resolutions).
Bounded text merging can be enabled explicitly as described below. Rename
detection and recursive base synthesis remain future work.

## Opt-in text composition {#opt-in-text-composition}

The C++ `NativeMergeOptions::text_merge` optional enables text composition with
the supplied `TextDiffOptions`. Leaving it empty preserves the default file-level
decisions and metadata-only input inspection. Explicit `resolutions` are applied
first; only remaining `content` conflicts are candidates for automatic text merge.
Add/add, delete/modify, binary data, overlapping edits and namespace conflicts
remain blocking. This does not change fast-forward representation or topology.

```cpp
syrnck::NativeMergeOptions settings;
settings.text_merge.emplace(); // Default text limits, explicit opt-in.
auto preview = store.plan_merge(project, "main", source_id, settings, true, operation);
auto result = store.merge_directory(project, "main", expected_tip, source_id,
                                    "Merge feature", settings, operation);
```

Each candidate's complete three inputs are reconstructed and hash-verified even
without full-history verification. File-size and batch byte ceilings are checked
before payload reads. At most 64 content conflicts are attempted, with a 16 MiB
aggregate charge for verified inputs plus clean results. Per-file work, line,
trace and serialized output limits also apply. Skipped or limited files keep
their original conflict and report the reason; the operation never chooses a
partial result to satisfy a budget.

The CLI exposes the same opt-in for both plan and apply:

```text
syrnck native merge-plan feature --text-merge --verify
syrnck native merge feature --text-merge --expect OLD_TIP_ID
syrnck native merge-plan feature --text-merge --max-file-bytes 2097152 --max-work 33554432
```

`--max-file-bytes`, `--max-lines`, `--max-work` and `--max-output-bytes` set the
per-file text budgets and require `--text-merge`. Plan and apply should use the
same settings; apply always recomputes against the expected current head.
English/French diagnostics and progress use the existing language and stderr
controls. JSON fields and reason codes stay language-independent.

The plan's `text_merge` object reports limits, attempt/byte counts, generated
manifest count and per-path status/ranges/target metadata. Disabled mode uses
`null`. Reports omit merged plaintext. Ready workspace actions refer to exact
predicted manifest IDs which need not exist in the Store yet. Planning does not
create any objects, including when one file composes and another remains blocked.
See [virtual file preparation](doc:text-merge#virtual-file-preparation).

Apply first recomputes the full plan, verifies input histories and checks all
workspace blockers. Only then does it materialize generated chunks/manifests,
requiring exact agreement with predicted IDs, and create the two-parent revision.
The existing recoverable checkout then publishes the complete result. Failure
before journal publication can leave unreferenced generated objects, with no
branch/workspace change; normal GC can collect them. Once a journal exists,
resume/abort uses its retained candidate without needing to rerun text merge.
Cancellation and observer exceptions preserve these boundaries.


## Preserve concurrent file contents {#conflict-copies}

```text
syrnck native merge-plan feature --text-merge --conflict-copies --verify
syrnck native merge feature --text-merge --conflict-copies --expect OLD_TIP_ID
```

`--conflict-copies` is an explicit file-level preservation policy. Explicit
`--resolutions` choices apply first; optional bounded text composition runs next.
Remaining content and add/add conflicts keep one version at the original path
and the other in a sibling copy. The lexicographically smaller complete revision
ID wins the original path, so reversing the two peers produces the same file
namespace. This ordering is deterministic; it does not imply newer or better data.
Concurrent delete/modify retains the modified file at its original path and
records `choice: keep_modified`. Both parent histories preserve the deletion.
Identical concurrent deletions still delete the file.

A sibling name combines at most 64 UTF-8 bytes of the stem, `.syrnck-conflict-`,
the losing revision's complete ID and 16 hex digits of the original path hash.
Extensions of at most 16 UTF-8 bytes, including `.uasset` and `.umap`, are kept.
No timestamp, hostname or random suffix influences the result. Reports include
`conflict_copies`, `resolved` entries with `choice: conflict_copy`, `copy_path`,
`copy_revision`, original conflict `reason` and both file descriptors.
The generated names are ordinary tracked files; review and remove them in a
later commit after choosing the desired asset version.

Any occupied generated name or parent in an input snapshot blocks with
`conflict_copy_collision`, including case aliases, directories and files that
would otherwise be deleted. Untracked/ignored working obstructions still block
checkout. Case and file/directory conflicts, ambiguous bases and unrelated
histories retain their normal policies. This option does not infer an edited
rename or compose binary data. Copies share the existing manifests and chunks;
there is no extra binary payload in the Store for the preserved version.
Apply verifies the complete input histories and uses the usual signed revision
and recoverable checkout. The frozen candidate, including its copy paths,
resumes without recomputing policy or requiring its signing key.

In C++, set `NativeMergeOptions::conflict_copies = true`. The default remains
false. Cherry-pick, revert and rebase keep their existing resolution policies.

## Reports and validation {#reports-and-validation}

The JSON report contains `mode`, `head`, `source`, selected `base`, all
`merge_bases`, graph traversal counts, `ready`, `objects_verified`, `result_files`,
proposed `changes`, file `conflicts`, accepted `resolved` choices,
`resolution_context`, topology `blockers` and `workspace` preflight.
With topology blockers, `result_files` and `workspace` are null. With file
conflicts, `workspace` is null. Otherwise its target ID is null because the plan
does not write a candidate revision. A ready plan exits 0; a blocked plan exits 1
with JSON on stdout. Invalid input or missing metadata produces a diagnostic.

`--verify` additionally verifies both input histories' complete closures and
reconstructed file hashes, including custom resolution manifests. Without it,
stored revision/manifest metadata is read
and working files are hashed, but incoming file block payloads are not audited.
Compact reports and candidate metadata are bounded as for replay. Memory scales
with the bounded ancestry graph, snapshots and report.

```cpp
syrnck::NativeMergeOptions settings;
settings.base = selected_best_base; // Optional explicit best ancestor.
auto plan = store.plan_merge(project, "main", source_id, settings, true, operation);
```

Planning supports cooperative cancellation and English/French progress/errors.
JSON fields and reason codes are language-independent. Callbacks must not reenter
the Store. Native merge is not exposed through the C ABI or Go binding yet.

## Apply and recover {#apply-and-recover}

```text
syrnck native merge feature --expect OLD_TIP_ID -m "Merge feature"
syrnck native merge release --ff-only
syrnck native merge feature --no-ff -m "Record feature integration"
```

Apply recomputes the plan, always verifies both input closures and reconstructed
file hashes, and requires the exact target tip inspected by the caller. The CLI
captures it under the Store lock when `--expect` is omitted. `--expect` accepts
an exact ID. The optional message defaults to `Merge SOURCE_ID`.

Up-to-date returns `state: unchanged` without writing objects or refs. Fast-forward
checks out the exact source revision without creating a new commit. Divergence
creates a revision with parents `[old_target, source]` and the supplied
message/current UTC timestamp. A [configured identity](doc:native-authorship)
creates v2 with the current person as author/committer and an optional signature;
otherwise it stays v1. A same-tree divergent history still creates this
two-parent commit. Source branches/tags and symbolic HEAD do not move.

`--no-ff` requests a two-parent commit even when a fast-forward is possible.
`--ff-only` refuses divergence with the `fast_forward_required` blocker. These
flags are mutually exclusive and work in both plan and apply. They do not bypass
base selection, unrelated-history opt-in, file conflicts or workspace preflight.

After preparing the candidate, apply uses the existing checkout journal, file
backups, atomic branch/archive/receipt publication and explicit recovery:

```text
syrnck native checkout-status
syrnck native checkout-recover
syrnck native checkout-recover --abort
```

Abort is available before branch publication; afterwards recovery only finishes
cleanup. An active journal blocks GC and new native history mutations. Candidate
metadata written before a failed/cancelled journal publication can remain
unreferenced. Opening the Store never applies workspace recovery automatically.
The [checkout data and durability limits](doc:native-checkout) apply, including
plaintext private prepared files/backups in encrypted Stores.

Both histories remain reachable through the new two-parent commit after a
divergent merge. Fast-forward already contains the old target in its ancestry.
Checkout archives additionally retain previous heads. These roots preserve old
file blocks through destructive GC. Reverting a merge uses the existing
`native revert MERGE_ID --mainline N -m MESSAGE` operation.

The direct result adds `operation: merge`, `mode`, `previous_revision`,
`merged_revision` and `base` to the checkout result. Recovery reports the generic
checkout journal identity and its recorded target; these response labels do not
change the journal format. Native revision v1 is unchanged.

```cpp
auto applied = store.merge_directory(project, "main", expected_id, source_id,
                                      "Merge feature", settings, operation);
```
