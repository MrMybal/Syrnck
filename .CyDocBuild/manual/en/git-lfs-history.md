---
id: git-lfs-history
title: Offline conversion of complete Git LFS history
description: Offline conversion of complete Git LFS history guide.
section: Git
order: 130
parent: git-assets
revision: 1
---

# Offline conversion of complete Git LFS history {#offline-conversion-of-complete-git-lfs-history}

`syrnck_git` can convert every commit reachable from one tip, including all
parents of merges, onto a new branch. Source references, HEAD, the index and
working files remain intact. No Git LFS server or `git-lfs` process is used.

```text
syrnck git lfs-history-preview main --progress
syrnck git migrate-lfs-history main --branch converted-history --workers 4 --progress
git log --graph converted-history
```

Both commands accept `--directory PROJECT`, `--lfs-objects CACHE/objects`,
`--max-commits 10000` and `--strip-signatures`. Migration uses the existing
Store's ingestion settings unless explicitly overridden. Repository filters
can be initialized separately with `syrnck git install`.

## Complete preflight {#complete-preflight}

Traversal reads raw commit parents, without replacements or history
simplification. Missing ancestors fail the operation; a shallow boundary is
never silently treated as a new root. The selected argument is resolved once
to an immutable commit before traversal. Moving the source branch later does
not change that selection.

Before importing assets, migration checks every distinct source tree's LFS
pointers and attribute updates, audits every distinct cached object with
SHA-256 and verifies existing Syrnck asset dependencies. Historical assets
deleted from the latest snapshot still need their cached bytes. Missing,
truncated and corrupt old-only objects prevent any candidate or Store write
during this preflight. The cache is checked again against the exact bytes
consumed by ingestion, so later cache changes cannot publish incorrect assets.

The read-only preview needs no Store or encryption key. It reports per-object
audit status, per-commit parents/tree, counts and whether signature removal is
required. Like the snapshot preview, it audits LFS cache data rather than the
availability of existing Syrnck assets or the final branch transaction.

## Preserved history and signatures {#preserved-history-and-signatures}

The rewrite preserves ordered parents, merges, roots, empty commits, raw
author/committer headers (including dates/time zones), encoding headers,
messages and other accepted extension headers. Unaffected commits keep their
original IDs. Every changed tree uses the same literal per-file attributes
and regular-file pointer conversion as [snapshot migration](doc:git-lfs-migration).
Unconverted code, executable modes, symlink blobs and Gitlinks keep their IDs.

A rewritten commit cannot retain its old signature. By default, any `gpgsig`,
`gpgsig-sha256` or `mergetag` header on a commit that needs rewriting blocks
migration. Review the preview, then explicitly permit removal if appropriate:

```text
syrnck git lfs-history-preview main --strip-signatures
syrnck git migrate-lfs-history main --branch converted-history --strip-signatures
```

These headers, including continuation lines, are removed only from rewritten
commits and counted in the result. Unaffected signed commits retain their exact
objects. There is no automatic re-signing, tag rewriting or mutation of other
branches. Repeat the operation for other tips when needed; CAS objects deduplicate
across runs, but references and signatures require their own deliberate policy.

## Retention and publication {#retention-and-publication}

For **every converted historical commit** containing Syrnck assets, migration
verifies and retains its full catalog at `git-assets/COMMIT` before publishing
the final branch. A later asset deletion, independent Git history cleanup,
Syrnck GC or packing therefore does not discard those protected versions.
Existing native assets are included alongside converted LFS assets. Empty
asset catalogs need no Store root.

The final new branch is created through Git's prepared reference transaction,
including the same symbolic-reference/worktree checks and user hook handling
as snapshot migration. It never replaces an existing branch. Cancellation is
honored before publication; after commit begins, an exact observed commit
reconciles a lost acknowledgement. An unresolved outcome returns exit 3 and
requires inspecting the reported reference and commit.

Failures or interruptions can leave valid unreferenced objects and already
prepared catalog pins. They are safe to reuse, but no migration journal
automatically deletes them. There is no atomic cross-system transaction
between Git and the Store: protection precedes branch publication. Upload of
blocks and Git publication remain separate operations.

Successful JSON contains the selected source, new tip/branch, counts,
`stripped_signature_headers`, `reconciled` and a `commits` array mapping each
source ID to its converted commit, tree and optional retained bundle. Keep this
mapping when reviewing or separately migrating tags. The local LFS cache is
never removed by migration.

## Bounds and validation {#bounds-and-validation}

The traversal defaults to 10,000 commits (configurable from 1 to 100,000), with
at most 1,024 parents per commit, 1 MiB per raw commit and a 64 MiB budget for
raw commit bytes plus estimated node metadata. Graph containers and allocator
overhead are additional memory. There are at most 65,536 distinct LFS objects;
the existing per-snapshot tree/attribute limits still apply. Both preview and
result JSON are bounded at 16 MiB. Payloads stream through the bounded native
pipeline; trees, graph metadata and mapping tables remain in memory. Git
subprocesses are currently invoked per commit/tree, so this is an offline
conversion tool rather than a tuned large-history server.


Git's [commit object format](https://git-scm.com/docs/gitformat-signature) and
[reference transactions](https://git-scm.com/docs/git-update-ref) define the
metadata and publication mechanisms used here.
