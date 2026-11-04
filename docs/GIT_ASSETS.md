# Git asset catalogs and retained bundles

Syrnck reads pointers from the **committed Git tree**, independently of dirty
working files, the index and checkout filters. Git keeps source code and its
normal history. A deterministic Syrnck revision describes only the assets needed
by one Git commit.

```text
syrnck git catalog HEAD
syrnck git catalog release --directory PATH_TO_REPOSITORY --progress-json
syrnck git prepare-assets HEAD
```

`catalog` is read-only and needs no Syrnck store or credentials. Its English JSON
contains `version`, the resolved full Git `commit`, and ordered `assets` entries
with `path`, Git `blob` ID, Syrnck `manifest` ID, logical `size` and `executable`.
SHA-1 and SHA-256 Git repositories are supported; annotated commit tags resolve
to their commit. The source revision is resolved once before reading the tree.

`prepare-assets` uses the repository's common Git store (`git-common-dir/syrnck`)
unless `--store` is supplied. Initialize filters/store first with `git install`.
It checks each pointer size against its manifest and verifies the full object
closure, including file hashes, before publishing `git-assets/COMMIT` locally.
A failure preserves existing pins. Different worktrees share the common store;
the Store's existing exclusive process lock still applies.

The bundle uses the existing revision format: no parents, timestamp zero,
message `Syrnck Git assets v1 COMMIT`, and asset paths/manifests/executable flags.
It contains no source-code files and does not replace a Git commit. Identical
catalogs produce identical bundle IDs. Path validation, portable case collisions
and the 16 MiB metadata limit apply. Bundle pins retain complete versions through
GC independently of later changes to Git refs. They currently require explicit
unpinning to release; automatic Git/ref/reflog retention reconciliation remains
future work.

## Bounds and local Git requirements

Use Git **2.45 or newer**. Local plumbing uses `--no-replace-objects` and
`--no-lazy-fetch`, so missing promisor objects fail instead of initiating a fetch.
[Git tree enumeration](https://git-scm.com/docs/git-ls-tree) uses NUL-delimited
paths and sizes; only regular blobs of at most 1,024 bytes can be pointer
candidates. Large ordinary Git blobs are never loaded into the catalog process.
Symlinks and submodule entries are excluded; submodules need their own setup.
A candidate starting with the exact Syrnck pointer header must parse canonically.
Other blobs are ordinary Git content.

The tree response is limited to 64 MiB and 262,144 entries. Unique candidate
blobs are read in batches of at most 128 using raw
[`cat-file --batch`](https://git-scm.com/docs/git-cat-file); each response is
limited to 160 KiB. Child stderr is limited to 16 KiB. The process helper spools
child output to temporary files before bounded reads; this does not cap temporary
disk usage or all metadata/RSS. Cancellation is checked between Git commands,
batch parsing and Store operations; a Git child already running may finish.

Git catalog fixtures cover dirty index/worktree independence, large ordinary
blobs, batching, store-less clones, symlink/submodule exclusions, SHA-256,
deterministic retained roots, malformed pointers, missing manifests and size
mismatches. Local execution is Windows x64; other platforms remain CI work.
