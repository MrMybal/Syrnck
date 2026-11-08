---
id: native-workspaces
title: Linked native workspaces
description: Independent working directories, HEADs and indexes over one shared Store.
section: Version control
order: 222
parent: native-branches
revision: 5
---

## Create and select a workspace {#usage}

```text
syrnck native workspace-add edit ../project-edit --branch feature
syrnck native workspaces
syrnck --store .syrnck --workspace edit native head
syrnck --store .syrnck --workspace edit native status --combined
syrnck --store .syrnck --workspace edit native add Source
syrnck --store .syrnck --workspace edit native commit --staged -m "Feature work"
```

The primary workspace retains the legacy layout. A registered linked workspace
has its own canonical directory, HEAD, frozen staging index, detached revision,
checkout receipts, bisect state, working cache and watch journal. Objects and
named branches remain shared; file data is not copied between Stores.

`workspace-add NAME DIRECTORY` runs from the primary context. The parent directory
must exist. An absent destination is created; existing files are preserved and
ordinary checkout collision rules apply. With `--branch NAME`, the branch must
already exist and be available. `--revision REVISION` initializes detached HEAD;
omitting both uses the primary committed revision, detached. An unborn primary
starts a linked workspace at a retained empty snapshot.

Names contain 1..64 lowercase ASCII letters, digits or internal hyphens. `primary`
and Windows device names are reserved. Registered roots must be disjoint from
each other, the primary workspace and the Store; links and portable aliases do
not provide an escape from these checks. At most 1,024 linked contexts are registered.

`--workspace NAME` selects a context for that invocation, with `primary` as an
explicit spelling of the default. Use `--store` to locate the shared Store when
running from another directory. Native workspace commands default `--directory`
to the registered root when a linked context is selected. An explicit directory
must resolve to that root. `native workspaces` includes the primary selection and
reports each linked directory, branch/revision and pending checkout.

## Partial working directories {#partial}

```text
syrnck native workspace-add code ../project-code --branch feature --include Source --include Config
syrnck --store .syrnck --workspace code native materialization
syrnck native workspace-add metadata ../project-metadata --no-files
```

Repeated `--include PATH` options select literal repository-relative files or
directory prefixes, using portable Unicode/case matching. Prefixes must be
disjoint; wildcards and exclusions are not supported. `--no-files` explicitly
selects an empty working directory. Omitting both options retains the complete
working directory. Root and ancestor `.syrnckignore` controls needed by selected
paths are included automatically with canonical spelling.

Revisions and frozen indexes always retain the complete logical file tree and
all its objects locally. Whole-directory commit, stage and amend preserve
omitted entries; working status, index and observation compare only the physical
selection. Explicit working-file edits outside that selection are refused.
Directory staging/commit requires an existing target revision to preserve omitted
entries; create a new branch from the complete selected revision first.
Snapshot diffs and `native export` still address the complete tree. Partial
materialization does not implement lazy block downloading or save Store space.

Checkout/switch, detached HEAD and portable restoration use the recorded
selection, including file/directory and case transitions. Excluded local files
are preserved; a namespace obstruction is refused. Backups retain the selection
and the complete index. Restoration materializes the selected index files and
verifies exactly that projected tree before binding the workspace.

The selection is immutable while the workspace remains registered. Create a
separate workspace for another selection. Hard reset and stash disk operations
currently require a complete workspace and fail before changing partial working
files. C++ creation uses `register_partial_native_workspace(primary, name,
directory, prefixes, operation)`; `native_materialization()` reports the policy.
C/Go can select and observe a partial context created by CLI/C++.

## Isolation and branch reservations {#isolation}

Staging in one workspace never consumes another workspace's index. Commits and
rewrite operations use the selected target. A named branch can be selected by
only one workspace at a time; switch plans report `branch_in_other_workspace`.
Branch changes through reference publication also check reservations, so an
explicit branch argument cannot move a foreign workspace's branch behind its
working files. Switch or detach that workspace first to release its branch.

One Store handle keeps its exclusive repository lock. Independent processes open
and close it around operations; watchers release it while waiting. A pending
checkout blocks new native mutations and GC across the Store until recovered in
the owning context. Filesystem application is journaled per workspace rather
than globally atomic across working directories.

## Observation {#observation}

```text
syrnck --store .syrnck --workspace edit native watch --combined --journal
```

Linked observation uses native root notifications where available and checks
the external Store's reference/configuration identities. Metadata publication
requests a complete observation without emitting paths outside the workspace.
Unsupported stamps or notification failures use polling. Periodic complete
audits remain necessary; notifications are advisory. Each context has separate
working-cache and journal files.

## Working-index benchmark {#performance}

```text
python scripts/benchmark_working_index.py --cli build/Release/syrnck.exe --linked --asset-mib 64 --iterations 5 --output linked-index.json
```

Omit `--linked` to measure primary; Unix builds use `build/syrnck`. The fixture
uses a synthetic 64 MiB file and verifies equivalent full/indexed status and a
same-size edit whose modification time is restored. On local Windows x64,
8 November 2026, five warm linked observations each reused one file and hashed
zero bytes; full observations each hashed 64 MiB. Median CLI times were 68 ms
full and 24 ms indexed (primary: 75 ms and 20 ms). The file was cached by the OS;
other regression tests were running. These measurements demonstrate avoided
reads on this fixture, not Unreal-project throughput or cross-platform latency.

```text
python scripts/benchmark_partial_workspace.py --cli build/Release/syrnck.exe --asset-mib 128 --iterations 5 --output partial-status.json
```

This separate fixture uses two synthetic assets totaling 128 MiB and 25 bytes of
code. Local Windows x64, 8 November 2026, five observations without the indexed
cache: the complete view hashed 134,217,753 bytes per observation, while the
`Source` view hashed 25 bytes. Median CLI times were 105 ms and 18 ms respectively.
The files had just been written/ingested, without flushing the OS cache; other
regression tests were running. The Store
retained all objects; the measurement concerns working status only.

## Backup and restoration {#restore}

```text
syrnck --store .syrnck backup create /backup/snapshot
syrnck --store /backup/snapshot backup restore /recovered/project/.syrnck
syrnck --store /recovered/project/.syrnck native workspace-restore edit /recovered/edit --directory /recovered/project
```

Backups omit active directory bindings and local checkout receipts. They retain
all indexes, detached data and completed target roots, plus a portable selection
descriptor and an exact revision root for each linked workspace. Backup format
v2 supports these exclusions for ungated experimental sources. A Store with
required reader features uses backup marker v3, which additionally binds those
requirements. V1/v2 backups remain readable.

`workspace-restore NAME DIRECTORY` binds a portable snapshot to a new directory.
It restores the original branch when its tip is unchanged and unreserved.
`--branch NEW_NAME` can create a rescue branch at the saved revision; `--detach`
restores independent HEAD. An occupied branch with another tip is never reset
implicitly. Frozen index content is materialized as working files, retaining
the committed HEAD separately. Unprepared dirty working bytes are not part of a
logical Store backup. Renaming the index's target archives its original snapshot
before publishing the replacement.

Restoration exclusively publishes the exported physical selection before binding
the context. If interrupted before binding, rerun the command: an existing tree
must match the selected saved content and modes. Changed exports, extra files,
links and protected metadata are refused without overwriting them. The Store
keeps the portable roots until an explicit retention operation removes them.

## Recovery {#recovery}

```text
syrnck native workspaces
syrnck --store .syrnck --workspace edit native checkout-status
syrnck --store .syrnck --workspace edit native checkout-recover
syrnck --store .syrnck --workspace edit native checkout-recover --abort
```

Registration atomically retains a valid empty detached state before initial
materialization. If initialization is blocked or interrupted, the registered
context remains inspectable; switch, checkout or recover in that context.
Registration does not move the primary HEAD or discard its prepared changes.
Checkout records bind their context and exact canonical directory; another
context cannot execute their recovery. Completed receipts remain local to their
context and are inspectable after later selections and GC.

The Windows x64 campaign verified all 808 observed IO process-termination
boundaries for linked registration and checkout entry/exit/movement, with plain
and encrypted Stores, resume/abort, namespace transitions, independent production
inspection and post-recovery GC. This tests process interruption with the OS and
filesystem running; power-loss behavior is a separate validation.

The partial-workspace campaign additionally verified all 716 observed process
termination boundaries for registration and checkout with the same plain/encrypted,
resume/abort, fresh-reader and post-recovery GC checks.

## C++, C and Go {#api}

```cpp
store.register_native_workspace(primary, "edit", directory, operation);
store.use_native_workspace("edit");
store.switch_directory(directory, "feature", operation);
auto changes = store.workspace_status(directory, store.native_target(), operation);
store.use_native_workspace(""); // primary, handle-local selection
```

C uses `syrnck_store_use_workspace` and
`syrnck_workspace_open_registered_ex`. A linked observation session requires a
Store selected to the same name. Selection failure preserves the previous name.
Go exposes `Store.UseWorkspace(name)` and `WorkspaceOptions{Name: "edit"}`;
the existing cancellation and serialized-handle rules apply. Empty names select
primary in these APIs. These are additive ABI 1 symbols requiring matching libraries.

## Formats and current scope {#formats}

Primary reference layouts and checkout records v1..v7 stay unchanged. Linked
metadata lives under real `workspaces/NAME/` references. Named indexes use the
BLAKE3 digest of the branch name in their slot, preserving the existing maximum
branch length. The retained snapshot still records the complete branch name.
Complete linked checkout records use v8 with `context` and `layout_version` (1..6).
Partial checkout records use v9 with frozen `source_materialization` and
`target_materialization` definitions, context and layout (1..5). Layout 5 supports
[recoverable partial hard reset](doc:native-hard-reset#partial-workspaces). The current
static selection requires equal source/target definitions. A canonical v1
materialization chunk is retained at `workspaces/NAME/workspace/materialization`.
Object/revision, pack and C ABI versions are unchanged.

Changing a live selection, partial stash, in-place workspace relocation
and archived unregistering remain work to complete. Every workspace still
requires its complete retained objects locally.

## Reader compatibility {#compatibility}

Registration and portable restore establish the `linked-workspaces-v1` reader
requirement before publishing a linked binding. Historical executables refuse
the Store, including ordinary ref commands, so they cannot bypass branch
reservations. Upgraded readers retain support for legacy configurations. See
[inspection and explicit upgrade](doc:settings#reader-features). Partial creation
also establishes `sparse-workspaces-v1` before publishing its policy. Readers
supporting linked contexts alone refuse this additional requirement.
