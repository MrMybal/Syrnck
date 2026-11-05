# Native merge planning

`native merge-plan` computes the topology and complete proposed file snapshot
without changing objects, references or working files. It shares file-level
three-way decisions with [native replay](NATIVE_REPLAY.md).

```text
syrnck native merge-plan feature --verify
syrnck native merge-plan feature --base BASE_ID
syrnck native merge-plan imported --allow-unrelated-histories
```

The default target is the selected native branch. `--branch` and `--directory`
override the operation's branch and workspace. The CLI accepts native revision
selectors; the C++ API takes exact source/base IDs. `--max-revisions` and
`--max-links` have the same bounds/defaults as [ancestry queries](NATIVE_HISTORY.md).
The complete input graph must be available; there is no shallow fallback.

## Topology

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
Text merging, rename detection and recursive base synthesis remain future work.

## Reports and validation

The JSON report contains `mode`, `head`, `source`, selected `base`, all
`merge_bases`, graph traversal counts, `ready`, `objects_verified`, `result_files`,
proposed `changes`, file `conflicts`, topology `blockers` and `workspace` preflight.
With topology blockers, `result_files` and `workspace` are null. With file
conflicts, `workspace` is null. Otherwise its target ID is null because the plan
does not write a candidate revision. A ready plan exits 0; a blocked plan exits 1
with JSON on stdout. Invalid input or missing metadata produces a diagnostic.

`--verify` additionally verifies both input histories' complete closures and
reconstructed file hashes. Without it, stored revision/manifest metadata is read
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
