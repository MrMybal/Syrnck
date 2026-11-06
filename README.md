# Syrnck

[English](README.md) · [Français](README.fr.md)

**A C++20 block storage and revision engine for large assets and differential file synchronization.**

Syrnck splits files into reusable, content-addressed blocks instead of storing a full copy after every change. Complete manifests describe each file version, so reconstruction does not depend on a chain of binary deltas.

Two modes share the same engine: **Git integration**, retaining Git's workflow for source code while storing assets as Syrnck pointers, and **standalone revisions**, building toward an independent version control system. The library is independent of Unreal Engine and can be embedded through C++ or its C ABI. A Go binding provides a foundation for future synchronization adapters.

**Status: working prototype with experimental formats.** The engine, Git filters, local revisions, server, and one-shot transfer agent run today. Standalone mode does not yet implement Git's full feature set. Unreal project benchmarks, an Unreal plugin, end-to-end encryption, and complete Syncthing daemon/folder interoperability remain future work.

## Components

| Component | Available functionality |
| --- | --- |
| `syrnck_core` | FastCDC/fixed blocks, BLAKE3, Zstd, verified file/range reads, chunk reuse plans, indexed packs, native staging/status/recoverable checkout/merge/replay/snapshot diff/tags/archived amendment, revisions, retention and GC |
| `syrnck_git` | Git filters, asset catalogs, retention inventory/protection, offline LFS migration and opt-in pre-push hooks |
| `syrnck_git_net` | Grouped Git asset push/pull, immutable catalogs and on-demand asset hydration |
| `syrnck_net` | Authenticated HTTP v1 API, missing-object comparison, differential transfer, and compare-and-swap reference publication |
| `syrnck` | Cross-platform CLI, loopback server, and push/pull agent |
| `syrnck_ffi` / `syrnck_ffi_static` | Shared/static C ABI for blocks, verified import, dependencies, file/range reads, reuse plans, pins and workspace observation |
| `bindings/go` | Go binding, verified ranges, file comparison, resumable differential copying and workspace sessions; Windows DLL / Unix cgo backends |

XChaCha20-Poly1305 protects objects **at rest**. An authorized server can read their contents. The HTTP server binds to loopback and is intended to sit behind an HTTPS proxy.

## Build and test

Requirements: CMake 3.25+, a C++20 compiler, a C compiler, and Ninja. On Windows, install the Visual Studio 2022 C++ tools and a Windows SDK. Linux/macOS also need OpenSSL **3.x** development libraries. Python 3.9+ runs the build helper and tests; the engine and CLI are native binaries.

```text
python scripts/build.py configure
python scripts/build.py build
python scripts/build.py test
```

The helper discovers MSVC/CMake/Ninja on Windows. In an initialized build environment, the presets can also be used directly:

```text
cmake --preset windows
cmake --build --preset windows
ctest --preset windows
```

Use the `unix` preset on Linux/macOS. Small dependencies are vendored; CMake downloads pinned Zstd and libcurl archives and verifies their SHA-256 hashes. See [third-party components and notices](THIRD_PARTY.md).

On macOS with Homebrew, install `ninja` and `openssl@3`. If multiple OpenSSL versions are installed, configure with `python scripts/build.py configure -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"`. The pinned curl requires OpenSSL 3.x; the network build rejects OpenSSL 4 instead of compiling against incompatible APIs.

The CLI is `build/Release/syrnck.exe` on Windows and `build/syrnck` with the Unix preset. Embed the C++ library through [core.hpp](include/syrnck/core.hpp), or use the [C ABI](include/syrnck.h). CMake targets are available through `add_subdirectory`.

## Quick start

With the binary directory on `PATH`, run these commands from a project directory:

```text
syrnck init
syrnck put Content/Map.uasset
syrnck native commit --message "Initial project snapshot"
syrnck refs
syrnck fsck
syrnck gc
```

`put` retains a file and prints its manifest ID and reuse statistics. `get MANIFEST NEW_FILE` reconstructs a file without overwriting an existing destination. `gc` is a dry run; `gc --delete` removes unreachable objects. Branches, revisions, archives, and pins retain their complete block dependencies, including shared blocks.

Use `get MANIFEST NEW_FILE --offset 1048576 --length 65536` to export a [verified byte range](docs/FILE_RANGES.md), reading only intersecting chunks. `compare OLD_MANIFEST NEW_MANIFEST` produces a [chunk reuse plan](docs/FILE_COMPARISON.md); `--summary` omits its per-block lists. Partial reads verify selected chunks; whole-file reconstruction also verifies the complete file hash. Comparison is a metadata operation, separate from payload verification and transfer.

```text
syrnck put large-file.bin --workers 4 --buffer-mib 32
syrnck config set --workers 4 --buffer-mib 32
syrnck native commit --message "Project snapshot" --progress
syrnck pack
syrnck pack --compact
syrnck --lang fr --help
```

Parallel compression/encryption is optional and uses a bounded queue; sequential ingestion remains the default. Indexed immutable packs reduce file counts. Ordinary packing appends new packs; explicit compaction regroups the store. Both use streaming pack plans and readback, without complete packed-ID lists; loose-only IDs still need sorting. See [compaction guarantees and memory limits](docs/COMPACTION.md). GC keeps unaffected packs and rewrites only partially reachable packs.

The verified object cache defaults to 64 MiB and 8,192 fingerprints. Every hit still checks the freshly read envelope; `fsck` and `gc` bypass decoded validation caches. The independent index page cache defaults to 4 MiB (`--index-cache-mib`). Both operations enumerate objects in a stream, with one mark bit per packed object; GC also rewrites survivors in a stream. Loose-only keys and metadata still consume memory proportional to their size. See [integrity audit guarantees and memory limits](docs/AUDIT.md).

Packing explicitly enables physical storage v2 for a single pack, or v3 for multiple packs. New stores remain v1 until packing; logical IDs and encrypted envelopes stay unchanged. Older readers reject unsupported physical formats. See [pack publication and recovery](docs/PACKS.md), [paged indexes](docs/INDEXES.md), and [reproducible measurements](docs/PERFORMANCE.md), currently documented in French.

`native diff-text SOURCE TARGET PATH` compares a verified file as bounded UTF-8 line hunks; `--unified` renders its text changes. Binary/limited results are explicit. See [text differences and limits](docs/TEXT_DIFF.md).

`native diff-text --staged PATH [--branch NAME]` compares frozen prepared bytes to their original baseline, including unborn or stale indexes. Later working changes are excluded; `--unified` is also available.

`native diff-text --working PATH` compares a working file to the frozen index (or branch head), with `--revision SELECTOR` for an explicit base. Reads target one path, reject ignored/obstructed names, and detect observed file changes. See [working text differences](docs/TEXT_DIFF.md#compare-a-working-file).

`native add-hunks-plan PATH` numbers the text changes of an existing file. `native add-hunks PATH --expect-plan ID --hunks 1,3` freezes only those hunks, preserves other preparations and archives the previous index through GC. See [partial text staging](docs/NATIVE_HUNKS.md).

`native reset-hunks-plan PATH` numbers changes already prepared against their baseline. `native reset-hunks PATH --expect-plan ID --hunks 1,3` removes selected hunks, preserves the remaining preparations and archives the complete old index. These commands use stored snapshots without reading working files.

`native add-lines-plan PATH` and `native add-lines PATH --expect-plan ID --lines 1,3` select individual added/removed rows within a hunk. `native reset-lines-plan` / `native reset-lines` remove prepared rows. Both preserve exact newline bytes and archive the previous index; see [individual line selection](docs/NATIVE_LINES.md).

`native add-patch-plan PATH --patch FILE` previews an external unified text patch against the frozen index/head. `native add-patch PATH --patch FILE --expect-plan ID` prepares its exact result with atomic old-index retention, without reading working files. See [strict text patches](docs/TEXT_PATCHES.md) for supported Git/native patches and bounds.

`native merge-text BASE CURRENT INCOMING PATH` previews a bounded, verified three-way text merge. `--text` emits exact bytes only for a clean result. Conflicts remain explicit; a reviewed result can be supplied through existing custom file resolutions. See [text merge and its limits](docs/TEXT_MERGE.md).

`native merge-plan feature --text-merge` enables bounded automatic text composition inside a native merge. Apply with `native merge feature --text-merge --expect OLD_TIP_ID`; explicit resolutions take priority and any remaining conflict blocks the complete merge. Plans prepare exact future manifest IDs without writing objects. See [automatic native text composition](docs/NATIVE_MERGE.md#opt-in-text-composition).

`native cherry-pick-plan`/`revert-plan` and their application commands also accept `--text-merge`. Revert removes only the selected delta while preserving later independent text changes. Results remain complete single-parent revisions with recoverable checkout and GC retention. See [bounded text replay](docs/NATIVE_REPLAY.md#optional-bounded-text-composition).

## Languages

Local ingestion and pack limits can be saved with `config set`; explicit command options take precedence, including native commits and Git clean filters. See [repository settings](docs/SETTINGS.md).

Imports, native commit/add/staged/reset/status/checkout-plan/checkout/checkout-recover/diff/export commands, reconstruction, `fsck`, Git filters and agent transfers accept `--progress` (English/French) or `--progress-json` on stderr. Ctrl+C requests cooperative cancellation (exit 130). Native exports prepare a complete tree before exclusive publication; existing destinations are preserved. A publication already started can complete. Remote publication with a lost/ambiguous response exits 3 so the caller can inspect the reference. See [operation contracts](docs/OPERATIONS.md), [transactional exports](docs/EXPORT.md) and [transfer recovery](docs/TRANSFERS.md).

`native status` hashes working files against a native branch or exact revision without ingesting them. `native diff SOURCE TARGET` compares snapshot metadata, including content, mode and manifest changes. See [native comparison contracts and limits](docs/NATIVE_COMPARISON.md).

`native status --indexed` persists an optional working hash cache with filesystem identity/change checks, recent-write fallback and automatic rebuild after GC. It supports combined staging status; destructive workspace preflights keep their full reads. See the [working index](docs/WORKING_INDEX.md).

`native watch` streams workspace changes with native Windows/Linux notifications, targeted file/subtree observation for edits and namespace changes, polling fallback, bounded event coalescing and full recovery/audit scans. It releases the Store between observations, can include staging with `--combined`, and saves optional recovery checkpoints with `--journal`. Inspect them with `native watch-journal`. See [resident workspace observation](docs/NATIVE_WATCH.md). [C and Go workspace sessions](docs/WORKSPACE_ABI.md) expose the same inventory and retained notifications for embedding without holding an idle Store lock. Workspace opening, waiting and status support cooperative cancellation; C callers can also receive progress callbacks.

`native add PATH...` freezes selected files and deletions; `native staged` inspects them, and `native reset [PATH...]` removes selections without changing working files. `native commit --staged -m MESSAGE` atomically publishes those prepared bytes and consumes the index. Staging roots protect prepared data against GC; a changed branch base requires an explicit reset. See [native staging and selected commits](docs/NATIVE_STAGING.md). Numbered text hunk selection is also available; see [partial staging](docs/NATIVE_HUNKS.md).

`native status --combined` separates staged and later working changes. `native checkout-plan REVISION` reports proposed actions and blockers, including ignored-file collisions, links, case aliases and pending staging. `--verify` additionally audits the target's complete stored closure. See [native workspace inspection](docs/NATIVE_WORKSPACE.md).

`native checkout REVISION` restores a clean tracked workspace and moves the selected branch, archiving its previous head and retaining the target. `native checkout-status` inspects an interrupted operation; `native checkout-recover` resumes it and `--abort` restores the originals before publication. File moves are sequential; the journal makes them recoverable. Prepared outputs and backups are plaintext even with encrypted objects. There is no force option or automatic checkout-history pruning yet. See [checkout guarantees, retention and recovery](docs/NATIVE_CHECKOUT.md).

Tracked file/directory conversions and file/parent case changes use a [namespace transaction](docs/NATIVE_CHECKOUT.md#recoverable-namespace-transitions): all changed sources are backed up before targets are installed, and abort removes targets before restoring originals. Untracked and ignored descendants block directory removal.

`native head` reports the current branch; legacy Stores implicitly select `main`. `native switch-plan BRANCH` previews a change, and `native switch BRANCH` restores and selects that existing branch without moving either branch head. Commit, staging, status, log and checkout default to the selected branch; `--branch` overrides it. HEAD publishes atomically with the recovery journal. See [native branch selection, defaults and recovery](docs/NATIVE_BRANCHES.md).

`native tag NAME [REVISION] [-m MESSAGE]` labels a committed version, defaulting to HEAD, with an optional annotation. `native tags` lists bounded pages; `tag-show --verify` audits the target. `tag-delete NAME --expect ID` retains a restorable archive through GC, and `tag-restore ARCHIVE_ID` restores the original label and message. Native snapshot commands accept HEAD, unambiguous branch/tag names and explicit `heads/NAME` or `tags/NAME`. See [native tags, selectors and archive retention](docs/NATIVE_TAGS.md).

`native commit --amend -m MESSAGE` replaces the tip while preserving its parents and atomically archiving the old revision. Add `--staged` to use prepared bytes or `--message-only` to reuse the existing snapshot; `--expect ID` guards the exact old tip. Archives retain their complete file blocks through GC. See [native amendment, retention and interruption recovery](docs/NATIVE_AMEND.md).

`native ancestry SOURCE TARGET` reports ancestry in both directions and every best common ancestor, including criss-cross histories, using a bounded read-only graph traversal. See [native revision ancestry](docs/NATIVE_HISTORY.md).

`native cherry-pick-plan REVISION` and `native revert-plan REVISION` compute file-level three-way replay, report conflicts and run the clean-workspace preflight without writing objects or files. Merge revisions require an explicit mainline parent. See [native replay decisions and limits](docs/NATIVE_REPLAY.md).

`native cherry-pick REVISION -m MESSAGE` and `native revert REVISION -m MESSAGE` create a child commit and apply its files through the recoverable checkout journal. They preserve independent changes, refuse unresolved file conflicts, and retain old versions through history and archives. `--expect ID` guards the old target tip; interrupted application supports resume or abort. See [replay application and recovery](docs/NATIVE_REPLAY.md#apply-a-planned-change).

`native merge-plan REVISION` distinguishes an up-to-date branch, fast-forward and divergent merge, selects a unique best common ancestor, and previews file conflicts and workspace blockers. Multiple bases require an explicit choice; unrelated histories require explicit opt-in. See [native merge topology and preflight](docs/NATIVE_MERGE.md).

`native merge REVISION` applies that result through the recoverable checkout journal: exact fast-forward or a commit with both histories as parents. `--ff-only`, `--no-ff` and `--expect ID` control integration; previous file versions remain retained after GC. See [merge application and recovery](docs/NATIVE_MERGE.md#apply-and-recover).

`--resolutions JSON_FILE` supplies explicit conflict choices to merge, cherry-pick, revert and their plans: current, incoming, base, removal or a custom imported manifest. Documents bind the exact revisions and selected parent; unresolved conflicts still block the complete operation. See [native conflict resolutions](docs/NATIVE_RESOLUTIONS.md).

`native reset-plan REVISION --soft|--mixed` previews a branch move; `native reset REVISION --soft|--mixed` applies it without writing working files. Soft preserves prepared entries on the new baseline; mixed archives and clears the index. Old history and staging blocks stay retained through GC. See [native branch reset](docs/NATIVE_RESET.md).

`native reset-plan REVISION --hard` previews replacing local work. Application
captures dirty files, the frozen index and untracked obstructions before their
replacement, with recover/abort and retained old bytes after GC. See
[recoverable hard reset](docs/NATIVE_HARD_RESET.md).

`native squash-plan COUNT` previews regrouping linear tip commits; `native squash COUNT -m MESSAGE` creates one commit with the exact previous tip snapshot. The complete old history remains archived through GC. Merge commits in the range and any staging index block the operation. See [native commit squash](docs/NATIVE_SQUASH.md).

`native archives` lists retained history in bounded pages, optionally filtered by reset, prepared index, squash, amendment, checkout or detachment. `native archive-show REFERENCE --verify` audits a stored version before export or recovery. See [native history archives](docs/NATIVE_ARCHIVES.md).

`native rebase-plan BASE --onto REVISION` previews replaying the explicit linear range `(BASE, HEAD]` on another base. It computes deterministic virtual commit IDs without storing candidates, preserves messages/timestamps and reports the first file conflict or workspace blocker. See [native linear rebase](docs/NATIVE_REBASE.md).

`native rebase BASE --onto REVISION --expect OLD_ID` creates the verified candidate chain and applies its final snapshot through the recoverable checkout journal. The complete old history stays archived through GC; source branches and tags remain unchanged. See [rebase application and recovery](docs/NATIVE_REBASE.md#apply-and-recover).

Use `native rebase-todo BASE --onto REVISION` to export an editable sequence,
then pass `--todo JSON_FILE`, or choose `--interactive` to enter it at the terminal.
Reordering, pick/drop/reword/squash/fixup preserve the original history archive.
See [editable and interactive rebase](docs/NATIVE_REBASE_TODO.md).

Rebase also accepts `--resolutions JSON_FILE`: a series of explicit file choices bound to the original head, both bases and each virtual replay step. Partial or stale choices block the whole operation. See [rebase conflict resolution](docs/NATIVE_REBASE.md#resolve-conflicts-by-step).

Add `--text-merge` to `rebase-plan`/`rebase` to compose independent UTF-8 edits across virtual steps. A shared budget covers the complete range; generated intermediate versions remain reconstructible after GC. See [bounded text rebase](docs/NATIVE_REBASE.md#optional-bounded-text-composition).

`native rebase-index-plan --verify` previews replaying a stale frozen staging index onto its current branch head, without reading working files or storing candidates. See [native staging rebase](docs/NATIVE_STAGE_REBASE.md).

`native rebase-index --expect HEAD_ID --expect-index INDEX_ID` publishes that prepared result atomically, retaining the former index and baseline through GC. Working files and branch selection remain untouched.

Both staging rebase commands accept `--resolutions JSON_FILE`, with explicit file choices bound to the branch head, frozen index and its original base. Custom manifests/modes are verified before publication; partial or stale choices cannot replace the index.

`rebase-index-plan`/`rebase-index --text-merge` also compose frozen text changes onto the current head, without reading working text. New manifests, the candidate index and the old index archive retain complete versions. See [text staging replay](docs/NATIVE_STAGE_REBASE.md#optional-bounded-text-composition).

`native branches --verify` lists bounded pages of branch heads and frozen staging state, auditing the displayed histories. `native branch NAME REVISION` now creates a fully verified head atomically and refuses existing heads, orphan indexes or active checkouts. See [native branches](docs/NATIVE_BRANCHES.md#branch-inventory-and-verified-creation).

`native branch-delete-plan` / `branch-delete --expect ID` remove inactive branches while atomically archiving their complete history and explicitly authorized staging indexes. `native branch-archives` / `branch-archive` inspect retained snapshots through GC. See [archived branch deletion](docs/NATIVE_BRANCHES.md#archived-branch-deletion).

`native branch-restore-plan ARCHIVE_ID` / `branch-restore ARCHIVE_ID [--as NAME]` recreate archived heads and frozen indexes atomically into absent slots. A new name changes only the index's branch marker; prepared file entries, modes and baselines stay intact. See [branch restoration](docs/NATIVE_BRANCHES.md#restore-an-archived-branch).

**English is the default. French is the second built-in interface language.** Select `--lang en` / `--lang fr`, or set `SYRNCK_LANG=en` / `SYRNCK_LANG=fr`. The command-line flag takes precedence. Operating-system locale settings do not change the default.

Help, status messages, and owned diagnostics are localized at the interface boundary. Commands, option names, JSON keys, object IDs, Git pointers, and network formats are language independent. Core exceptions and C/Go diagnostics retain their canonical English text; C++ interfaces can use [i18n.hpp](include/syrnck/i18n.hpp) for presentation. Unknown operating-system/dependency diagnostics fall back to their original text. See [localization policy](docs/LOCALIZATION.md).

## Git and synchronization

Completed workspace receipts have [explicit backup-fenced retirement](docs/CHECKOUT_RETENTION.md),
including restored target roots and optional source archives.

Native history includes [recoverable stash save/apply/pop](docs/NATIVE_STASH.md),
with independent working/index snapshots, explicit conflict plans and an unchanged branch head.

Native history also supports [persistent no-checkout bisection](docs/NATIVE_BISECT.md),
with merge-aware candidate selection, skipped-build ambiguity and retained history.
[`native diff --renames` and `native log-file`](docs/NATIVE_FILE_HISTORY.md) add
exact-content rename detection and paged first-parent file lineage.

`backup create`, `backup verify`, `backup restore` and `backup repair` provide
[verified offline snapshots and missing-object repair](docs/BACKUPS.md), with
new destination publication, optional envelope key changes and preservation of
retained history. Restore exports working files separately; this is not live
replication or end-to-end encryption.
`backup repair-copy-plan` / `backup repair-copy` add a
[verified new Store from damaged current storage and a backup](docs/REPAIR_COPY.md),
preserving newer intact history and recovering corrupt loose/packed envelopes.
`backup retire-plan` / `backup retire` add [backup-protected archive retirement](docs/ARCHIVE_RETENTION.md).

`locks acquire-plan`/`acquire`, `renew`, `release`, `list` and `prune` provide
[repository-local asset leases](docs/ASSET_LOCKS.md), with atomic groups, expiry
and persistent fencing generations. [Configured server access](docs/SERVER_ACCESS.md)
adds authenticated owners, remote leases, repository roles, write-path permissions
and branch publication checks. `native push` transfers autonomous history;
`native fetch` updates archived tracking references without changing working files.

In an existing Git repository, run `syrnck git install`, then add the appropriate rules from [examples/unreal.gitattributes](examples/unreal.gitattributes). An opt-in [pre-push hook](docs/GIT_ASSETS.md#opt-in-pre-push-hook) can verify and upload assets throughout the history being pushed before Git proceeds. Install it explicitly and configure a separate block URL. [Checkout policies](docs/GIT_ASSETS.md#on-demand-checkout-and-pointer-only-clones) support verified local reconstruction, opt-in downloads of missing blocks, and pointer-only clones for code work. The [resident filter](docs/GIT_FILTER_PROCESS.md) keeps one Store and its bounded verified cache across files in a Git command. Deferred hydration and a watcher remain future work.

Existing LFS projects can use `syrnck git lfs-preview HEAD` to audit cached objects, then `syrnck git migrate-lfs HEAD --branch converted-assets` to create a child commit with native pointers and retained blocks. This preserves HEAD, the index and working files. It converts one snapshot; earlier commits still require LFS storage. See [offline LFS migration and recovery](docs/GIT_LFS_MIGRATION.md).

For all reachable versions, use `git lfs-history-preview HEAD` followed by
`git migrate-lfs-history HEAD --branch converted-history`. This preserves merge
topology and raw author/committer metadata, retains every historical asset
catalog and requires an explicit decision about signatures on rewritten commits.
See [complete offline LFS history conversion](docs/GIT_LFS_HISTORY.md).

`syrnck git retention-scan` inventories pointers across references, reflogs and worktree indexes. `git retention` previews verified protections; `git retention --apply` adds them atomically. Existing pins remain intact; unobserved pins are reported for inspection. See [Git retention coverage and limits](docs/GIT_RETENTION.md).

The project takes inspiration from [Lore](https://github.com/EpicGames/lore) for chunked storage and [Syncthing](https://github.com/syncthing/syncthing) for differential synchronization. No implementation code from either project was copied into this engine. An optional [Syncthing adapter](bindings/syncthing/README.md) supports regular-file BEP blocks and differential reconstruction, tested against its reference library over TLS 1.3. Full daemon/folder synchronization remains future work.

Agent transfers accept `--requests 4 --transfer-buffer-mib 64` to overlap object requests while keeping Store access coordinated. The default remains one request; the buffer budget covers object payloads and reserved responses, rather than total process memory. See [parallel transfer limits and recovery](docs/PARALLEL_TRANSFERS.md). Packing and GC support progress/cancellation during preparation, then finish committed cleanup; see [maintenance boundaries](docs/MAINTENANCE.md).

Transient requests retry up to three times by default (`--retries 0..8`). Optional `--upload-kib-s` and `--download-kib-s` targets share object bandwidth across parallel requests; zero means unlimited. Retry and pacing waits are cancellable. The final reference publication is never retried automatically. See [retry and traffic policy](docs/TRANSFER_POLICY.md).

Transfers persist local recovery records. Use `agent sessions`, `agent resume URL SESSION_ID` and `agent forget SESSION_ID` to inspect, resume or deliberately discard them. Unfinished sessions retain their complete source or available downloaded objects during local GC, and resume preserves saved reference expectations. `agent prune` previews completed receipts; `agent prune --delete` removes them while preserving unfinished records, references and payloads. See [persistent sessions and publication recovery](docs/TRANSFER_SESSIONS.md) and [selective retention](docs/SELECTIVE_RETENTION.md).

Compatible servers also retain uploads with expiring, renewable leases and fenced publication. Use `--require-lease` to require protection; offline `leases list`, `leases release ID` and `leases prune` manage server records. Publication receipts recover lost acknowledgements without reverting later reference changes. See [server upload leases](docs/SERVER_UPLOAD_LEASES.md).

New uploads negotiate [paged inventories](docs/UPLOAD_INVENTORIES.md), registered before presence checks. Server GC retains declared blocks even before the manifest arrives and can collect unrelated data. Legacy leases keep their global hold; `leases prune` also cleans obsolete inventory generations.

Pulls also acquire renewable [read leases](docs/SERVER_READ_LEASES.md) for the exact source root before downloading missing blocks. They survive server restarts and reference removal until expiry or release. Use `--require-read-lease` to require support, `serve --read-lease-seconds 3600` to set their TTL, and `leases ... --read` for offline maintenance.

Object transfers negotiate [Zstandard compression](docs/NETWORK_COMPRESSION.md)
by default. Use `--compression auto|off|zstd` or `serve --no-compression`.
Incompressible data can travel raw; bounded decoding preserves object IDs and
verification. Results distinguish logical bytes from observed object-body traffic.

## Validation and repository publishing

Local builds and tests have been run on Windows x64 with MSVC. CI is configured for Windows, Linux, macOS, and Linux sanitizers; configuration alone does not establish that those remote runs have passed. The C++ implementation needs no Rust compiler or runtime. Compatibility fixtures exercise the original v1 Rust format, including encrypted stores.

The [process interruption suite](docs/CRASH_RECOVERY.md) kills native writers at publication and cleanup boundaries, then reopens and resumes with the production core. It covers retained-file reconstruction in plaintext and encrypted stores. An interrupted GC may require completing its loose-object cleanup before a full `fsck` succeeds; the recovery guide explains this state. These tests exercise process termination with the OS running, rather than power-loss durability.

Run [Gitleaks](https://github.com/gitleaks/gitleaks) before committing or uploading:

```text
python scripts/scan_secrets.py --gitleaks PATH_TO_GITLEAKS
```

The script scans a snapshot of the publishable working tree, including vendored dependencies, while leaving build outputs, local tools, stores, and scan reports out of Git. [GitHub publishing instructions](docs/PUBLISHING.md) describe the first import into `MrMybal/Syrnck`, authentication, staged/history scans, and CI. This Git source upload is independent of Syrnck's experimental asset transfer protocol.

## Further reading

- [Localization and contribution rules](docs/LOCALIZATION.md) — English
- [Large reference tables and measured listing performance](docs/REFERENCE_TABLES.md) — English
- [GitHub publishing and repository description](docs/PUBLISHING.md) — English
- [Usage](docs/USAGE.md), [architecture](docs/ARCHITECTURE.md), [formats](docs/PROTOCOL.md), [roadmap](docs/ROADMAP.md), [performance](docs/PERFORMANCE.md), and [migration](docs/MIGRATION_CPP.md) — French
- [Go binding](bindings/go/README.md) — French

## License

Syrnck's original code is licensed under the [MIT License](LICENSE).
Third-party components retain their respective licenses and notices; see
[THIRD_PARTY.md](THIRD_PARTY.md).
