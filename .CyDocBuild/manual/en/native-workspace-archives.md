---
id: native-workspace-archives
title: Archive and restore a workspace
description: Unregister linked workspaces while retaining dirty files, prepared changes and history.
section: Version control
order: 223
parent: native-workspaces
revision: 1
---

## Unregister a linked workspace {#unregister}

```text
syrnck native workspace-unregister-plan edit --include-untracked --verify
syrnck native workspace-unregister edit --include-untracked --expect-plan PLAN_ID
syrnck native workspace-archives --verify
syrnck native workspace-archive ARCHIVE_ID --verify
```

Use the exact `plan` from the preview, with the same `--include-untracked`
choice. The commands select the named linked context themselves. The primary
workspace cannot be unregistered, and pending checkouts must be recovered first.
The preview reads files and references without publishing objects or metadata.
Changed captured contents, selection or context references invalidate the plan.

Application captures the physical working state and retains a canonical archive
before removing the registration and every `workspaces/NAME/` reference in one
reference transaction. It leaves the original directory and all its files in
place and releases the branch reservation. Shared branches and primary HEAD
remain unchanged. The C++ Store handle returns to the primary context.

## What is retained {#contents}

The archive keeps three independent states:

- The complete committed HEAD, including files outside a partial selection.
- The complete frozen index, when present, with its original parent, even if stale.
- A physical recovery revision containing selected tracked files, their local
  changes and regular-file obstructions to the logical tree. Deletions remain
  absent from that physical snapshot. `--include-untracked` additionally captures
  selected nonignored regular files discovered by the native scanner.

Every original context reference is retained through an explicit typed archive
root, including inactive indexes and completed recovery/history references.
GC, packing and [backups](doc:backups) preserve their object closures.
Ignored and unselected local files remain in the original directory; they are
not promised in the recovery snapshot. Empty directories are not versioned.
Unsafe paths, links or protected metadata can block a plan; inspect `ready` and
the reported blockers before applying it.

The inventory accepts `--after ID`, `--limit N` (1..1000, default 100) and
`--verify`. IDs are stable archive object IDs; pagination orders them by ID.
`workspace-archive` displays the saved reference recipe and physical revision.

## Restore into a new directory {#restore}

```text
syrnck native workspace-archive-restore-plan ARCHIVE_ID recovered ../recovered --detach --verify
syrnck native workspace-archive-restore ARCHIVE_ID recovered ../recovered --detach --expect-plan PLAN_ID
syrnck --store .syrnck --workspace recovered native status --combined
```

Run restoration from the primary context. The destination's parent must exist;
the destination must not overlap the primary, Store or another registered
workspace. An existing directory is reused only if its regular files and modes
match the complete saved physical snapshot. Extra or changed files and links
are refused. An existing exact snapshot may contain additional empty directories.

The original named or detached selection is the default. `--branch NEW_NAME`
restores under another branch; `--detach` creates a private HEAD. A named branch
must be absent or still point at the saved HEAD and be unreserved. Restoration
never resets an advanced branch. HEAD retains the committed revision, the
working directory contains the captured dirty bytes, and the prepared index
remains separate. Changing its selector rewrites only the index message; it
does not silently rebase a stale index.

The physical export is published before registration. Retry the same command
and plan after interruption: an exact completed export is reused, and an exact
completed registration is idempotent. Changes to files or bindings are refused.
The saved partial selection is restored, while physical ancestor obstructions
are restored in full rather than filtered out. Earlier inactive context roots
remain accessible through the archive; they are not reactivated in the new name.
The new context also pins its physical recovery revision independently.

## Backups and archive retirement {#retention}

```text
syrnck backup create /existing-backup-parent/with-workspace-archive
syrnck backup retire-plan archive/workspaces/ARCHIVE_ID --from /existing-backup-parent/with-workspace-archive
syrnck backup retire archive/workspaces/ARCHIVE_ID --from /existing-backup-parent/with-workspace-archive --expect-plan RETIRE_PLAN
```

Retirement requires the verified backup to retain the exact complete archive
group. A member such as `/record`, `/head`, `/work` or `/roots/...` cannot be
retired individually. Retirement removes roots, without deleting working files
or running GC. A restored context keeps its physical recovery pin even after
the source archive is retired. Its selected HEAD/index remain ordinary roots;
inactive original context references then depend on the verified backup or
other surviving roots. See [archive retention](doc:archive-retention).

For recovery after local GC, restore that backup into a separate Store, then
run `workspace-archive-restore-plan` and `workspace-archive-restore` there with
the recovered primary directory supplied through `--directory`. Historical
absolute source bindings are recipes, not active registrations; the original
directory need not exist.

## C++ API and current limits {#api}

```cpp
#include <syrnck/workspace.hpp>
store.use_native_workspace("edit");
auto plan = syrnck::plan_native_workspace_unregistration(store, true, true);
auto archived = syrnck::unregister_native_workspace(
    store, plan["plan"].get<std::string>(), true);
auto id = archived["archive"].get<std::string>();
syrnck::NativeWorkspaceRestoreOptions settings;
settings.detached = true;
auto restore = syrnck::plan_native_workspace_archive_restore(
    store, primary, id, "recovered", destination, settings, true);
syrnck::restore_native_workspace_archive(store, primary, id, "recovered",
    destination, restore["plan"].get<std::string>(), settings);
```

The header also exposes archive inspection and paginated inventory. Operations
accept cancellation and progress callbacks. Files and refs are rechecked after
the last publishing callback, without calling observers during final publication.
External writers still require coordination; this is not an atomic filesystem
snapshot. Process-interruption tests do not establish power-loss durability.
In-place relocation and automatic archive age/release policies remain separate
work. Encrypted Stores protect archived objects at rest; working exports and
their temporary files have the usual plaintext workspace semantics.
