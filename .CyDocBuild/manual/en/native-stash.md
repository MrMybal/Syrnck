---
id: native-stash
title: Native stash
description: Native stash guide.
section: Version control
order: 390
parent: native-history
revision: 1
---

# Native stash {#native-stash}

Native stash captures complete working files and the frozen staging index before
cleaning the selected branch's workspace. Applying it later restores local work
without advancing the branch. The API is in `syrnck/stash.hpp` and has no network
or CLI dependency.

```text
syrnck native stash push -m "WIP: lighting"
syrnck native stash push --include-untracked -m "WIP: new assets"
syrnck native stash list --limit 100
syrnck native stash show STASH_ID --verify
syrnck native stash apply-plan STASH_ID --index --verify
syrnck native stash pop STASH_ID --index --expect-plan PLAN_ID
```

Use the canonical 64-character stash ID returned by `push` or `list`. There are
no mutable positional aliases. `--directory` selects the workspace; the Store
must be inside it. `--branch` must name its selected branch. Existing pending
checkout/switch/reset/stash operations must first be recovered.

## Save and scope {#save-and-scope}

Saving includes paths tracked by the head or frozen index, regardless of ignore
rules, and regular untracked files obstructing a tracked namespace transition.
`--include-untracked` additionally captures regular files selected by the shared
native scanner, respecting `.syrnckignore`. Ignored unrelated files stay in place.
Links/reparse points, special files, protected Store/Git data and ambiguous names
are refused when they obstruct the operation; the optional scanner also refuses
selected links/reparse points. Its directory depth limit is 128.

The complete working snapshot has the old head and optional frozen index as
parents. Its own files retain working contents, while the separate index parent
retains staged contents even when they differ. Missing tracked paths remain
missing in the saved work. The cleanup target is the current head. Empty
directories are not versioned in revision snapshots, though the reset journal
retains observed directories needed to abort a namespace transition.

Stash metadata and a monotonically increasing generation counter publish
atomically with the initial version 5 hard-reset journal and snapshot pins.
Working files only move after that publication. Failure before publication can
leave valid unreferenced CAS objects; interruption afterward leaves a retained
stash and recoverable cleanup. Aborting cleanup restores the original workspace
and index, while keeping the stash as an additional recovery copy. A clean save
without an index returns `stash: null` and writes nothing.

`list --before GENERATION --limit N` pages newest first. It reads and validates
active metadata and paired work references without auditing all file payloads.
Messages in list entries are previews of at most 256 UTF-8 bytes; `show` returns
the full message. `show --verify` audits the complete saved closure. JSON keys,
IDs, user messages and operation names are stable across English/French output.

## Reapply, conflicts and index {#reapply-conflicts-and-index}

`apply-plan` is read-only. Application requires its exact `--expect-plan` ID and
recomputes the plan before writing candidates. A dirty tracked workspace,
existing frozen index, active checkout or obstructing untracked data blocks
application. It does not overwrite existing local work.

Saved working files merge against their original head and the currently selected
head. With `--index`, staged files merge separately against the **original
index's own parent**, preserving later head changes even when the saved index
was stale. The new index is based on the current branch head. Without `--index`,
staged changes remain part of the working result without creating a frozen index.

Conflicting file edits block the entire operation. Optional `--text-merge` uses
the existing bounded, explicit text merge engine. Binary files still require a
choice. The plan exposes separate `working.resolution_context` and
`index.resolution_context` documents:

```text
syrnck native stash apply-plan STASH_ID --index --text-merge
syrnck native stash apply-plan STASH_ID --index --resolutions work.json --index-resolutions index.json
syrnck native stash apply STASH_ID --index --resolutions work.json --index-resolutions index.json --expect-plan PLAN_ID
```

Fill the contexts' `files` maps using the existing `current`, `incoming`, `base`,
`remove` or explicit verified `file` choices. Operations are named `stash` and
`stash-index`; head/source/base/parent IDs bind choices to the selected inputs.
Text merging shares the existing aggregate attempt and byte limits across both
results. Repeat the same merge/resolution options when applying a plan.

The version 6 checkout journal distinguishes the published branch head from the
working target. The branch head remains unchanged. Final publication installs
the optional new index and, for `pop`, archives/removes the stash in **one CAS**
with the committed receipt. `apply` keeps the active stash. The retained working
revision also keeps the original stash and new index reachable after GC.

```text
syrnck native checkout-status
syrnck native checkout-recover
syrnck native checkout-recover --abort
```

Before final publication, abort restores the clean pre-apply workspace and leaves
the active stash available. After publication, recovery completes cleanup. Plain
working/prepared/rollback files follow the same local confidentiality limits as
[ordinary checkout](doc:native-checkout). Store opening never resumes implicitly.

## Drop, restore and retention {#drop-restore-and-retention}

```text
syrnck native stash drop STASH_ID --expect-record RECORD_ID
syrnck native stash archive STASH_ID --verify
syrnck native stash restore STASH_ID
syrnck native archives --kind stash --verify
```

`drop` is fenced by the immutable record ID and moves the active metadata/work
references to archive references. `pop` uses the same archival protection. Neither
deletes objects. Restore preserves the original generation and requires that the
active ID is absent. Archived records and work roots survive backup/restore and
GC. Work roots can be explicitly retired with [verified backup coverage](doc:archive-retention);
a retired stash cannot be restored from the live Store without restoring its
archive protection/data from a retained backup. Independent checkout archives
and completed receipts can continue to retain shared objects.

## Bounds and verification {#bounds-and-verification}

At most 4,096 active stashes and 1,000 list rows per page are supported. Messages
are at most 1 MiB. Existing 16 MiB revision, inventory and report bounds apply.
The engine streams file bytes through the configured ingestion pipeline. It
still inventories/hashes files; this is separate from a persistent working index
or watcher.


On Windows hosts with the legacy canonical path-length limit, deep paths can
be refused by the OS before the scanner reaches 128 levels. Full Windows long-path support is still an independent compatibility task.
