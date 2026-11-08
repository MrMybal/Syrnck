---
id: native-detached
title: Detached native HEAD
description: Explore revisions and commit independently of named branches.
section: Version control
order: 221
parent: native-branches
revision: 1
---

## Select a detached revision {#usage}

```text
syrnck native switch-plan --detach tags/release --verify
syrnck native switch --detach tags/release
syrnck native head
syrnck native commit -m "Detached experiment"
syrnck native branch rescue HEAD
syrnck native switch main
```

`switch --detach REVISION` restores a committed snapshot without selecting or
moving a named branch. It accepts the same revision selectors as checkout.
`native head` reports `branch: null`, `mode: "detached"`,
`reference: "workspace/detached"` and the committed revision. No fake branch
appears in `native branches`. This differs from `detach-history`, which creates
a new ancestry root for the selected target.

Default native commits, staging, status, history, rewrite operations, stash and
workspace observation use the detached revision and its independent index.
Commits parent the previous detached revision. An explicit `--branch NAME`
continues to operate on that named branch without selecting it. Native remote
publication requires a named branch; create a rescue branch before publishing.

`native checkout REVISION` moves the currently selected target: a branch when
attached, the private revision when detached. `native switch NAME` returns to
a named branch and restores its files; neither branch tip moves during switch.

## Preflight and recovery {#recovery}

Plans are read-only. Dirty tracked files, an index on either target, untracked
collisions and unsafe namespaces block detachment or reattachment under the
ordinary checkout rules. There is no implicit overwrite or index reset.
File/directory transitions use the same recoverable namespace executor.

Before detaching an attached workspace, the destination is verified and retained
in a private slot. Replacing an inactive detached line archives its previous
tip under `archive/OLD_ID`. An interruption during this preparation leaves the
original HEAD and working files intact, with an additional retained candidate.
Once the checkout journal exists, use `native checkout-status` and
`native checkout-recover` (or `--abort`). HEAD changes with the committed journal.

## Retention and formats {#retention}

The private revision root `workspace/detached` and optional index root
`workspace/staging` retain complete object closures through GC. Switching away
keeps the detached tip. Archives, rescue branches and tags retain older lines;
portable backups retain these private data roots while omitting machine-bound
HEAD. Usual explicit retirement rules still apply to archives.

Named HEAD descriptors keep canonical version 1. Detached descriptors use
version 2 with `branch: null` and the same directory/type fields. Checkout
records involving a detached target use version 7 with `layout_version` naming
the corresponding layout 1 through 6. Named-only records keep their existing
formats. Detached stash records use version 2; named stash records stay version 1.
Older executors must refuse the new records rather than misread their targets.
Object/revision formats, physical pack formats and C ABI versions are unchanged.

## Embedding {#api}

`Store::native_target()` returns a named branch or the opaque selector `:HEAD`.
Pass it to native APIs that take an operational branch/target argument. Snapshot
APIs still take revision IDs; use `resolve_native_revision("HEAD")` first.
`:HEAD` cannot be a real branch name. Legacy plan/status fields named `branch`
contain this target selector when detached, while `native_head().branch` is null.
The C/Go workspace APIs follow detached HEAD when their selector is omitted.

```cpp
auto plan = store.plan_detached_checkout(workspace, revision, true, operation);
if (plan["ready"].get<bool>())
  store.detach_checkout(workspace, revision, operation);
auto target = store.native_target();
auto changes = store.workspace_status(workspace, target, operation);
```

One canonical workspace is still selected per Store. Linked/partial workspaces
and workspace relocation remain separate work.
