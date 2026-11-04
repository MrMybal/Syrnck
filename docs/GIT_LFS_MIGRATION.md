# Offline Git LFS migration

Syrnck can convert the Git LFS assets of one committed snapshot into native
Syrnck pointers. It creates one child commit on a new branch. The source commit,
HEAD, index and working files are preserved, including uncommitted changes.

This is a snapshot migration. Earlier commits still contain their original LFS
pointers and need their original LFS storage to reconstruct those versions.
Rewriting an entire history, downloading LFS objects and converting LFS extension
pipelines are separate, unimplemented operations.

## Prepare and inspect

Make the source snapshot's LFS objects available locally using your existing LFS
tools or an offline copy. Syrnck never contacts an LFS server and does not invoke
`git-lfs`. Inspect the committed pointers and verify their cached contents:

```text
syrnck git lfs-preview HEAD --progress
```

The preview needs Git, but no Syrnck Store or encryption key. It reads committed
blobs directly, so a `git clone --no-checkout` is sufficient for the Git side.
The default cache is `lfs/objects` beneath the Git common directory, shared by
linked worktrees. Each object must have the usual layout:

```text
OBJECTS_DIRECTORY/OID[0:2]/OID[2:4]/OID
```

For a copied cache or a repository with custom `lfs.storage`, supply the objects
directory explicitly. Syrnck refuses to guess the meaning of a custom storage
setting. Relative overrides are resolved against the process working directory.

```text
syrnck git lfs-preview HEAD --directory PROJECT --lfs-objects CACHE/objects
```

JSON lists each path, Git blob ID, LFS SHA-256 OID, declared size, executable
flag and status: `verified`, `missing`, `size_mismatch` or `hash_mismatch`.
Duplicate OIDs are hashed once. `ready` is true only for a nonempty catalog
whose LFS objects all verify. Exit status is 0 when ready, 1 otherwise.
Malformed pointers and unsafe cache entries fail the operation. The cache root,
its two shard directories and object files must not be symbolic links; ancestors
above the supplied root are not checked for symbolic links.

The preview audits LFS content only. It does not establish that a target branch,
attribute files, Git author identity, existing Syrnck assets or user hooks will
permit migration.

## Convert a snapshot

Initialize the Syrnck Store and repository filters, then choose an unused branch:

```text
syrnck git install
syrnck git migrate-lfs HEAD --branch converted-assets --workers 4 --buffer-mib 32 --progress
git diff HEAD converted-assets
```

`git install` writes local filter configuration as a separate setup step. The
migration itself does not change Git configuration or install hooks. It requires
an existing Store, a local Git author identity and all source LFS objects.
`--directory` and `--lfs-objects` work as for the preview. Without pipeline
overrides, the CLI uses the Store's saved ingestion settings.

For each unique LFS object, migration streams its bytes through the production
chunking, compression and optional encryption pipeline. SHA-256 covers the exact
bytes consumed by that pipeline; both the consumed size and digest must match
the original pointer. Native manifests and blocks keep their existing BLAKE3
identities and formats. A repeated asset reuses the imported manifest.

The new Git tree replaces only the selected LFS pointer blobs and necessary
`.gitattributes` blobs. Each converted path receives a literal, anchored basename
rule in its immediate parent directory, after existing rules:

```text
"/Map.uasset" filter=syrnck -text -diff -merge -ident -working-tree-encoding -eol -crlf
```

This overrides inherited LFS selection and disables text transformations for
that asset. Spaces and brackets are escaped. Other files and unchanged subtrees
retain their Git object IDs; executable bits, symbolic-link blobs and submodule
entries are preserved. Only regular-file LFS pointers are converted. A changed
attributes file must be a regular file, contain no NUL bytes and fit the size
limit. An LFS-managed `.gitattributes` file cannot be converted by this operation.

Before publication, Syrnck verifies the new commit's entire native asset catalog,
including any Syrnck assets already present in the source snapshot. It retains
the complete bundle under `git-assets/NEW_COMMIT`. GC and packing can then
preserve all of its dependencies independently of Git history retention.

Finally, Git prepares a reference transaction and holds the target reference
lock. Syrnck checks the reference again while that lock is held, then commits
the new branch. This also protects dangling symbolic references on older Git
versions that treat their target OID as absent. Existing references and branches
selected by an unborn HEAD or linked worktree are refused. Existing Git
`reference-transaction` hooks run and can reject publication; commit-tree does
not invoke ordinary commit hooks or request commit signing.

Successful JSON includes `source`, `commit`, `branch`, `converted_assets`,
`unique_objects`, the retained `bundle`, the converted `assets` and `reconciled`.
Review the branch and check it out explicitly when ready. Git source publication
and Syrnck block upload remain separate operations; see [Git asset transfers](GIT_ASSETS.md).
The existing LFS cache is read only and remains in its original form even when
the Syrnck Store is encrypted at rest.

## Cancellation and recovery

Both commands accept `--progress` or `--progress-json` on stderr. Ctrl+C requests
cooperative cancellation, with exit 130 before publication starts. Child Git
commands and hooks can delay cancellation; a blocked pipe read must return
before the operation can proceed.

Once the final Git commit command has begun, migration finishes or reconciles
publication without further cancellation checks. A late interrupt can therefore
return success with the branch created. If acknowledgement is lost, an exact
reference match to the prepared commit returns success with `reconciled: true`.
An unresolved publication returns exit 3: inspect the reported reference and
commit before retrying. Never infer failure to publish from a lost response.

Failures before bundle retention can leave unreferenced, valid CAS objects;
normal `gc --delete` can collect them. A prepared bundle pin remains after later
failure or cancellation, protecting its blocks even if no branch was created.
Git may also retain unreachable blobs, trees or the prepared commit. There is no
migration journal or automatic deletion of prepared pins. Inspect `refs`, the
reported Git reference and its catalog before deliberately removing an unused
pin. A retry can reuse already imported objects and requires an absent target
branch. Neither failure nor success deletes the LFS cache.

## Library and limits

[git_lfs.hpp](../include/syrnck/git_lfs.hpp) exposes pointer parsing, catalog
inspection, cache resolution, preview and migration through `syrnck_git`, with
no HTTP dependency. C++ callers provide an already open Store and optional
`IngestOptions`; callbacks follow the [operation contract](OPERATIONS.md).
SHA-256 uses the privately vendored [PicoSHA2 dependency](../THIRD_PARTY.md).

The implementation currently limits:

- Git tree output to 64 MiB and 262,144 entries, including directories.
- Pointer candidates to regular blobs of at most 1,024 bytes, read in batches
  of 128 with a 160 KiB response cap. Accepted canonical LFS pointers are
  strictly smaller than 1,024 bytes; legacy versions, extensions and unknown
  fields are refused.
- A migration to 65,536 asset paths, 4,096 changed attributes directories and
  1 MiB per changed attributes file. Native bundle metadata must also fit its
  existing 16 MiB format limit.
- SHA-256 scratch batches to 64 KiB. Asset ingestion uses the existing bounded
  block pipeline without a complete asset spool; tree metadata, visited IDs,
  parsed pointers and change maps still grow with the snapshot.
- Transaction response lines to 4 KiB and each acknowledgement's diagnostic
  output to 16 KiB. Git stderr uses a temporary file; user hook execution time
  and that file's size do not have a separate quota.

Regression coverage includes independent Python SHA-256 vectors, SHA-1/SHA-256
Git repositories, encrypted Stores, duplicate assets, corrupt/missing cache
objects, mixed native assets, dirty workspaces, real linked-worktree checkouts,
GC/packing, user hooks, reference races and cancellation around publication.
The private native probe also injects acknowledgement loss after a real Git
commit, checking exact reconciliation and preservation of a subsequent writer.
Local execution is validated on Windows x64; Unix runtime validation remains
part of the platform CI work.

Pointer semantics follow the [Git LFS specification](https://raw.githubusercontent.com/git-lfs/git-lfs/main/docs/spec.md).
Tree and reference writes use Git's [mktree](https://git-scm.com/docs/git-mktree)
and [update-ref transactions](https://git-scm.com/docs/git-update-ref).
