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

## Grouped transfers

```text
syrnck git push-assets https://blocks.example.invalid HEAD --requests 4 --require-lease
syrnck git pull-assets https://blocks.example.invalid HEAD --requests 4 --require-read-lease
```

The local Git commit must already exist. These commands use the common Git store
unless `--store` overrides it. Tokens/keys remain in their usual environment;
`--ca`, compression, retries and shared traffic/buffer limits work as for the
ordinary agent. The result contains `commit`, `assets`, `bundle`, and the nested
`transfer` statistics/session receipt.

Push prepares the verified bundle, transfers its deduplicated closure using the
existing persistent upload inventory/lease protocol, and publishes the single
server reference `git-COMMIT`. That reference is immutable through this command:
an existing different root fails before transfer rather than being overwritten.
The final server CAS verifies the complete closure. Another commit, including a
change only to source code, has its own reference. Git itself is not pushed by
this command. A repeated upload reuses verified objects and session recovery.

Pull reconstructs the expected bundle ID from the locally committed pointers,
then requires the matching server reference. It downloads into
`git-stage/BUNDLE_ID`, verifies all pointer size contracts and file hashes, and
publishes `git-assets/COMMIT` only after those checks. The staging pin is then
removed. A failed validation/interruption can retain a staging pin and journal;
rerun `git pull-assets` after fixing the cause to finish validation/publication.
An explicit `agent resume` only finishes its underlying staging transfer.

No working file or Git index is modified by grouped transfer. For a fresh clone,
use `git clone --no-checkout`, run `syrnck git install`, pull the desired commit's
assets, then explicitly check out that commit. This is an explicit hydration
workflow; automatic on-demand smudging and `filter-process` remain future work.

The transaction covers one catalog's publication. Multiple Git commits use
separate roots, and the Git server and block server do not participate in one
distributed transaction. A failed Git publication can leave retained block roots.
Do not delete those roots while corresponding Git history must stay reconstructible.

## Opt-in pre-push hook

```text
syrnck git install
syrnck git configure-remote origin https://blocks.example.invalid
syrnck git install-hook
git push origin main
```

The hook is installed only by the explicit `install-hook` command. Ordinary
filter installation does not install it. The block URL is stored locally as
`remote.origin.syrnckUrl`, separately from Git's transport URL. CLI configuration
rejects credentials/query/fragment URLs and non-loopback HTTP. The generated
script contains only quoted executable/store paths, inherits token/key environment
variables, and forwards Git's hook arguments/stdin. No token is written to Git
config or the script. The automatic invocation uses system CAs; for private CAs,
integrate the CLI invocation with `--ca PATH` into an existing hook manually.

Git's [pre-push input](https://git-scm.com/docs/githooks#_pre_push) identifies each
local tip and proposed remote update. The hook resolves non-deleted tips to
commits and walks their **entire locally reachable ancestry**, including merge
parents, using [`rev-list`](https://git-scm.com/docs/git-rev-list). It does not
assume assets are present merely because Git advertises an old remote commit.
Annotated commit tags and overlapping multi-ref pushes share this traversal.
An asset removed from the newest tree still needs publishing for its historical
commit. Each nonempty catalog is verified, uploaded and pinned before the hook
allows Git to continue. Upload-lease support is required for this hook.

The block client is created lazily. Deletion-only and entirely code-only histories
need no block endpoint/token or block network request. A current tree with no
assets does not bypass checks on older asset-bearing commits. Missing manifests,
bad pointer sizes, unavailable blocks, wrong immutable roots, unsupported leases
and network errors fail the hook and abort that Git push. Rerun the push after
fixing the cause; verified uploads and publication receipts are reusable.

Installation is atomic and idempotent for the exact generated script. An existing
different hook, a symlinked hooks path or configured `core.hooksPath` is refused
without overwriting user files. Existing hook workflows must call the equivalent
`syrnck --store COMMON_STORE git pre-push "$@"` themselves, preserve stdin, and
propagate its failure. Shared Git worktrees use the common hooks/store location.

This is a **local client hook**, and Git's `--no-verify` bypasses it. It does not
enforce a policy on GitHub/GitLab or make Git/block publication atomic across
servers. Cancellation or a later Git push failure can leave complete block roots
retained, and a multi-ref Git push uses Git's own atomicity rules. Server-side Git
enforcement, automatic orphan-root expiry and distributed publication remain
future work.

The current hook rejects shallow/grafted histories and tags which cannot resolve
to commits. Fetch a complete local Git history and its pointer blobs before use.
Missing promisor data fails locally. It accepts at most 4,096 update lines,
1 MiB stdin, 4 KiB per line and 100,000 unique reachable commits; history output
is limited to 8 MiB. It rescans committed trees on each invocation and does not
claim incremental production throughput. Presence/catalog indexing remains work
for larger projects.

The `git_pre_push` suite runs real pushes to isolated **local** bare Git repos
and an encrypted loopback block server. It checks intermediate/deleted asset
versions, annotated tags and multiple refs, old-version clone/checkout, missing
historical blocks, unavailable endpoints, retry, existing-hook preservation,
SHA-256 linked worktrees, code-only/deletion flows and invalid/shallow/grafted input. It does not push this
source repository or contact GitHub.

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
