# Git retention inventory and verified protection

Syrnck can inspect the assets still observable through Git and add complete
retention roots before local block maintenance. Inspection is explicit; it does
not run automatically inside Git filters, packing or GC.

```text
syrnck git retention-scan --progress
syrnck git retention --progress
syrnck git retention --apply --progress
```

All commands accept `--directory PATH`. The scan needs Git and no Store, key,
checkout filters or working-file contents. The other commands use the common
Git Store by default; `--store` can select an initialized custom Store.

## What the scan observes

Each registered worktree is traversed explicitly through Git, including the
main worktree or bare repository. The traversal covers:

- All references and their ancestors, including branches, remote-tracking refs,
  annotated/lightweight tags, stash and private worktree refs.
- All objects mentioned in each worktree's reflogs and their reachable history.
- HEAD, including detached HEAD, and all indexed objects, including conflict
  stages and an index in an unborn repository.

Scanning each worktree explicitly also covers private refs that Git 2.45's
global traversal omits from other worktrees. Shared objects are deduplicated
before inspecting blob contents. Ordinary large blobs are filtered from history
traversal; only blobs of at most 1,024 bytes are read as pointer candidates.
Object IDs and sizes are inspected in batches, without executing clean/smudge.

Any canonical Syrnck pointer-shaped blob in that object set is included,
regardless of attributes, path or file mode. Blob tags and symbolic-link blobs
can therefore retain extra assets conservatively. Duplicate manifest IDs are
merged; contradictory declared sizes or malformed pointers fail the scan.
No LFS object is converted or fetched by this command.

Unstaged working-file contents, untracked files, arbitrary alternate indexes,
unregistered worktrees, submodule repositories and operational pseudo-refs such
as `ORIG_HEAD` or `FETCH_HEAD` are not additional roots. Already retained pins
remain protected. Run separate scans for submodule repositories as needed.

The scan returns English JSON with `version`, a path-derived `repository`
identity, an object-set `fingerprint`, counts for `objects` and `commits`, and
sorted `manifests` containing ID and size. This is a metadata inventory, not
proof that local Syrnck blocks exist. The common Git directory's canonical path
determines the namespace; moving a repository can create a different namespace.

## Preview and apply

`git retention` reads the inventory and current Store references, then verifies
every observed manifest's declared size, complete block closure and whole-file
hash. It returns the scan fields plus:

- `add`: missing per-repository pins and their object keys.
- `unobserved_pins`: existing pins not observed through this inventory, with
  `repository` or `legacy` scope labels.
- `ready` and `verification_error`: whether all observed assets verified;
  a verification error is canonical English diagnostic data.
- `applied` and `added`: whether the requested update completed and its count.

The default preview changes nothing. Missing or corrupt assets produce
`ready: false`, exit 1, and no additions, including with `--apply`. Fetch or
restore the missing content separately, then retry. A successful empty scan is
ready and needs no new pins.

With `--apply`, the inventory is read again before publication. A changed object
set aborts with a retry diagnostic. New roots use:

```text
git-live/REPOSITORY_ID/MANIFEST_ID
```

The new roots are verified again and added in one compare-and-swap reference
transaction. Either every addition is published, or none is. Existing matching
pins are reused; a conflicting object under this namespace is an error.
User references, native branches, transfer roots and existing Git pins are
preserved. A repeat with no additions returns `applied: true, added: 0`.

The command writes neither Git history nor HEAD, index, configuration or working
files. It makes no network request and invokes no Git publication hooks.

## Unobserved does not mean safe to delete

This release only adds protections. It never removes pins automatically.
`unobserved_pins` is an inspection report, not a deletion plan or a reclaimable
byte estimate. Legacy `git/MANIFEST` and `git-assets/COMMIT` pins may belong to
another repository sharing the Store, or intentionally preserve deleted history.
Old per-repository pins also remain until deliberately removed.

Git and Syrnck do not share one transaction lock for refs, reflogs, indexes and
worktrees. Rechecking detects a change between the two scans, but does not freeze
Git after the second scan. Additions safely retain the captured snapshot; they
cannot promise coverage of concurrent or later Git changes. Coordinate Git
activity and rerun before maintenance that relies on this inventory. Dirty
working files do not become retained versions through inspection alone.

Automatic retirement needs further coordination and a recovery policy. Until
then, inspect the report and retain desired versions before deliberate manual
unpinning. Ordinary `gc` remains a preview; `gc --delete` only collects objects
unreachable from all remaining pins and other supported retention roots.

## Transactions, cancellation and limits

The C++ [Store API](../include/syrnck/core.hpp) exposes `RefUpdate` and
`compare_update_refs(updates, operation)` for atomic multi-reference updates.
Each update has a name, optional expected old key and optional new key. Null
expected means absent; null new key means removal. Duplicate names are errors.
An expected-value mismatch returns false without changing references. All new
closures and file hashes are verified before publishing one `refs.json` through
the existing atomic writer. Other references are preserved. Callers must
serialize Store access, including callbacks, as for other Store methods.

The references document has a 16 MiB write limit, checked before publication.
Known-unique, sorted keys are emitted without repeated linear duplicate searches.
An oversized transaction leaves the previous document intact and readable.

All Git retention commands support text/JSON progress and cooperative
cancellation. Scanning counts object entries; verification uses the existing
closure progress contract. Cancellation before reference publication leaves
pins unchanged. There are no callbacks or cancellation checks after publication;
an operation already committed can complete successfully after an interrupt.
Child Git commands must finish before a pending cancellation can be observed.

Current bounds are 4,096 worktrees, 1 MiB worktree-list output, 524,288 unique
Git objects, 65,536 unique manifests and 64 MiB traversal output per worktree.
Metadata headers are read in batches of 512 (64 KiB response cap); small blob
bodies in batches of 128 (160 KiB cap). Ordinary blob payloads are not retained
in memory across batches. IDs, sorted sets, fingerprints, pins and result JSON
still grow with repository history. A full shared history is traversed once per
worktree; there is no persistent incremental inventory yet.

Shallow histories, graft history and environment overrides `GIT_NAMESPACE`,
`GIT_INDEX_FILE`, `GIT_SHALLOW_FILE`, `GIT_DIR`, `GIT_WORK_TREE`, `GIT_COMMON_DIR`
and `GIT_OBJECT_DIRECTORY` are refused. Missing/inaccessible registered worktrees
or unavailable Git objects fail before publication. Lazy object fetching,
replace objects and fsmonitor are disabled for the inspection commands.
Traversal output on stderr is also refused, including warnings about reflogs
that reference pruned commits even when Git exits successfully. Disable Git
tracing or repair the reported Git metadata before retrying.

Regression tests cover Git SHA-1/SHA-256, encrypted Stores, bare/storeless scans,
unborn indexes, private refs and reflogs, detached linked worktrees, conflict
stages, historical assets, blob tags, missing blocks, malformed metadata,
changed inventories, cancellation, atomic updates and GC reconstruction.
Local execution is Windows x64; Unix runtime validation remains pending CI.

Traversal follows Git's [rev-list object options](https://git-scm.com/docs/git-rev-list)
and [batched object inspection](https://git-scm.com/docs/git-cat-file).
