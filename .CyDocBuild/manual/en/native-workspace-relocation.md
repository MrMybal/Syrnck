---
id: native-workspace-relocation
title: Move a linked workspace
description: Recoverable directory relocation, including case changes and unversioned local data.
section: Version control
order: 224
parent: native-workspaces
revision: 1
---

## Preview and move {#usage}

```text
syrnck native workspace-relocate-plan edit ../project-edit
syrnck native workspace-relocate edit ../project-edit --expect-plan PLAN_ID
syrnck --store .syrnck --workspace edit native head
```

Run from the primary context and use the exact preview's `plan`. The source must
be a registered linked workspace. The destination's parent must exist and be on
the same filesystem; the destination must be absent. An existing case alias of
the source is allowed for a change such as `project-edit` to `Project-Edit`.
An independently occupied case variant is refused on case-sensitive filesystems.

The operation moves the entire directory object, including dirty and staged
working files, ignored and unselected files, empty directories and descendant
links. It does not traverse links, reconstruct files from HEAD or reingest
working data. Logical HEAD, index, partial selection and shared branches stay
unchanged. Only the linked registration and HEAD directory binding change.
The primary directory and Store remain in place.

Planning reads directory identities and metadata without writing objects or
changing files. Its identity binds the complete reference table, old bindings,
source directory identity and both parent identities. It does not freeze file
contents: edits inside the same directory object move with that object. Keep
editors, watchers and other tools coordinated during the operation.

## Resume or cancel a pending move {#recovery}

```text
syrnck native workspace-relocation-status
syrnck native workspace-relocation-recover
syrnck native workspace-relocation-recover --abort
```

Use the primary context for recovery, even if the old linked directory is
temporarily absent. A pending transaction identifies the physical location as
`source`, `bridge` or `destination`. Forward recovery reaches the destination;
abort returns the same directory object to its original path before retaining
the original bindings. Neither path recursively deletes working data.

The journal is retained before the first filesystem move. Every relocation uses
a unique sibling bridge named `.syrnck-relocate-NONCE`, including changes of
case. Exclusive renames refuse an occupied destination. Registration and HEAD
publish together only after the physical destination has the saved directory
identity. After interruption, recovery inspects actual identities rather than
assuming a rename completed. Do not remove or repurpose the bridge.

Rerunning `workspace-relocate` with the same name, destination and plan resumes
its matching pending move. A completed matching operation returns its retained
receipt when the current bindings and destination identity still match. A changed
binding or replaced directory requires inspection; it is not silently adopted.
Status/recovery return `pending: false, state: none` when no transaction is active.

## Safety and scope {#safety}

The `workspace-relocation-v1` reader requirement is enabled before journaling.
Older readers refuse the Store. During a pending move, other reference changes,
native mutations, GC and backup creation are blocked until recovery finishes.
Directory and parent replacements, root links/reparse points, ambiguous identity,
overlaps and foreign occupied paths are refused. A raw reference edit cannot
rebind a live linked registration around this protocol.

Directory identities use Windows volume/file IDs or POSIX device/inode IDs.
Unsupported identity providers fail closed. Final callbacks run before renewed
identity/reference checks, with no observers inside the final publication.
This coordinates Syrnck writers; arbitrary external filesystem writers still
require coordination. Process-interruption checks do not establish power-loss
durability or qualify every network filesystem.

Cross-filesystem moves and relocation of the primary workspace or Store are
not implemented by these commands. Completed relocation receipts remain
retained; their age-based cleanup is not automated. Store backups retain
logical objects and receipts, while unprepared local working bytes still need
an explicit [workspace archive](doc:native-workspace-archives) or a filesystem
backup. Moving a directory does not turn its plaintext working files into
encrypted files.

## C++ API {#api}

```cpp
#include <syrnck/workspace.hpp>
store.use_native_workspace("");
auto plan = syrnck::plan_native_workspace_relocation(store, "edit", destination);
syrnck::relocate_native_workspace(store, "edit", destination,
    plan["plan"].get<std::string>(), operation);
auto pending = syrnck::native_workspace_relocation_status(store);
syrnck::recover_native_workspace_relocation(store, false, operation);
```

Pass `true` for abort. These are C++ operations; the C and Go interfaces keep
their existing ABI. Progress/cancellation may leave a pending move, whose
retained journal is required for subsequent resume or abort.

## Journal format {#format}

`workspace-relocations/active` retains a canonical native-workspace-relocation
v1 chunk with fourteen fields: `version`, `type`, `name`, `nonce`, `source`,
`destination`, `bridge`, `identity`, `source_parent`, `destination_parent`,
`registration`, `head`, `refs_hash`, `plan`. Bindings are stored inline, without
hidden object dependencies. Existing context roots remain in place while pending.
The central reference validator accepts only the exact start/finalization
transitions. Finalization removes the active root and adds
`archive/workspace-relocations/ID/completed` or `/aborted`, retaining the record.
Normal object, revision, pack and C ABI versions remain unchanged.
