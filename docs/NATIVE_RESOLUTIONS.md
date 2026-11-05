# Native conflict resolutions

Native merge, cherry-pick and revert share explicit file-level conflict choices.
Their plan commands remain read-only. Supplying choices does not bypass topology,
object verification, clean tracked files or checkout recovery requirements.

## Prepare and apply

```text
syrnck native merge-plan feature --verify
syrnck native merge-plan feature --resolutions choices.json --verify
syrnck native merge feature --resolutions choices.json --expect OLD_TIP_ID
```

Copy the first plan's `resolution_context` object into `choices.json`, then fill
its `files` map with the desired choices. Preserve every other field. A blocked
plan exits 1 with its JSON on stdout. Merge topology blockers must be resolved
first; their `resolution_context` is null. Keep the same base/mainline options
when previewing and applying the choices.

The same `--resolutions JSON_FILE` option works with `cherry-pick-plan`,
`cherry-pick`, `revert-plan` and `revert`. The application commands recompute
the plan and verify all input histories and custom file payloads before applying
the complete result through the existing recoverable checkout journal.

The document binds the operation, exact target `head`, exact `source`, three-way
`base` and selected replay `parent`. Merge has a null `parent`; cherry-pick's
base is its selected parent; revert's base is the source revision and its incoming
snapshot is the selected parent. Root replay has a null parent. Changing the
target, source, merge base or selected replay parent rejects the document before
publication. In particular, changing a merge revert's `--mainline` cannot reuse
choices made for another parent. Branch/tag selectors are resolved to exact IDs.

Example document, with illustrative IDs to replace using the actual plan:

```json
{
  "version": 1,
  "operation": "merge",
  "head": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
  "source": "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
  "base": "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
  "parent": null,
  "files": {
    "Content/Map.uasset": {"choice": "incoming"},
    "Content/Obsolete.uasset": {"choice": "remove"}
  }
}
```

## Choices

| Choice | Result for the path |
| --- | --- |
| `current` | Complete current branch entry, including its manifest and executable mode |
| `incoming` | Complete incoming entry; for revert this is the selected parent's version |
| `base` | Complete baseline entry used by the three-way decision |
| `remove` | Delete the path from the result |
| `file` | Use an existing Store manifest and the explicitly supplied executable mode |

Selecting an absent side deletes the path. Non-`file` entries contain only
`choice`. A custom entry has exactly these three fields:

```json
{"choice": "file", "manifest": "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd", "executable": false}
```

To combine content manually, save the resolved file outside the tracked workspace,
import it with `syrnck put PATH`, then use the returned `manifest` ID in this
entry. `put` alone does not retain an object against GC: keep a pin/reference to
an imported custom manifest if maintenance may run before application. Completed
commits retain their selected manifests and blocks through their normal history
and archive roots, including encrypted and packed Stores.

Only paths involved in the original unresolved plan's file conflicts or either
side of its namespace collisions can have choices. Overrides of clean paths are rejected.
The engine reapplies portable case/prefix collision checks after all choices.
An explicit choice can therefore still leave a namespace or workspace blocker.
Existing checkout limitations on case renames and file/directory transitions
continue to apply.

Partial resolutions keep the whole operation blocked; compatible files are not
published separately. The report's `resolved` rows show accepted choices and
their resulting entry, while `conflicts` contains remaining conflicts. An
all-current replay with no remaining logical delta is empty and cannot create
an empty commit. A divergent merge can record its two parents with unchanged
file contents.

## Validation and recovery

Without `--verify`, the plan reads custom manifest metadata but does not audit
its block payloads. `--verify` and application check the complete custom file,
including its reconstructed hash. Missing or damaged selected data prevents
publication. Plan callbacks/cancellation have the existing read-only contracts.

Editable JSON is limited to 16 MiB, 100,000 paths, depth 64 and 400,010 total
object fields. The root contains exactly the seven documented fields. Unknown
fields, duplicate keys, invalid paths/IDs and incorrectly typed choices are
rejected. The C++ typed API applies the same schema and conservative size budget;
use `decode` for untrusted editable JSON to detect duplicate keys before a DOM
can discard them. IDs are lowercase 64-character hexadecimal values.

Resolution documents do not create persistent conflict state, modify tracked
files or retain their input objects. Keep the necessary branch/tag/pin roots
until application. Automatic text merge, rename detection and an interactive
conflict editor remain future work. Recovery after application starts uses
`native checkout-status` and `native checkout-recover [--abort]`; it finishes or
rolls back the recorded candidate without needing the original JSON document.
Revision and checkout-journal formats are unchanged. See
[merge recovery](NATIVE_MERGE.md#apply-and-recover) and
[replay retention](NATIVE_REPLAY.md#interruption-abort-and-retention).

```cpp
auto choices = syrnck::NativeResolutionSet::parse(plan["resolution_context"]);
choices.files["Content/Map.uasset"] = {syrnck::NativeResolutionChoice::incoming};
syrnck::NativeMergeOptions settings;
settings.resolutions = choices;
auto preview = store.plan_merge(project, "main", source_id, settings, true, operation);
auto applied = store.merge_directory(project, "main", expected_id, source_id,
                                      "Resolve asset merge", settings, operation);
```

Replay uses `plan_replay_resolved` and `replay_directory_resolved` with a context
obtained from the corresponding replay plan; existing replay API signatures
remain available. These features are exposed through C++ and CLI; C ABI and Go
native history bindings remain future work.

Tests cover every choice, stale context/mainline rejection, incomplete choices,
namespace collisions, strict JSON decoding, read-only plans, missing custom
payloads, plain/encrypted and loose/packed storage, replay and reconstruction
after GC. Process-kill tests also cover resolved merge publication and rollback.
