---
id: architecture
title: Architecture
description: Architecture guide.
section: Getting started
order: 10
revision: 5
---

# Syrnck architecture {#architecture-de-syrnck}

One engine supports large assets, standalone revisions and block reuse for file synchronization. Read the workflow guides for exact contracts and the [limits](doc:limits) for unfinished work.

## Goals and decisions {#objectif-et-decisions}

Git keeps ordinary code diff/merge/rebase/blame/tags/hooks/review. Standalone commands develop those workflows over shared immutable objects. C++20/CMake provide native libraries, CLI, C ABI and Go integration. The storage engine has no Git/HTTP dependency; adapters depend on it. RAII and operation ownership govern lifetimes.

The static architecture diagram requires no interactive graph or 3 D dependency. [Lore’s design](https://epicgames.github.io/lore/explanation/system-design/) also separates storage from revision control; this does not make formats compatible.

```diagram
{"type":"flow","title":"Syrnck","nodes":[{"id":"git","label":"Git / filters","x":0,"y":0},{"id":"cli","label":"CLI / native history","x":250,"y":0},{"id":"ffi","label":"C / Go / BEP adapter","x":500,"y":0},{"id":"core","label":"C++20 core","x":250,"y":140},{"id":"cas","label":"CAS / manifests / revisions","x":0,"y":280},{"id":"refs","label":"Refs / retention","x":250,"y":280},{"id":"net","label":"HTTP / block server","x":500,"y":280}],"edges":[{"from":"git","to":"core"},{"from":"cli","to":"core"},{"from":"ffi","to":"core"},{"from":"core","to":"cas"},{"from":"core","to":"refs"},{"from":"net","to":"core"}]}
```


## Files and revisions {#fichiers-et-revisions}

Typed domain-separated BLAKE3 IDs identify chunks. Complete manifests contain total size, file hash, chunking policy and ordered full block lists. Sharing requires no parent relation. Revisions contain a complete sorted path→manifest/executable snapshot, parents, message and timestamp. Paged Merkle metadata remains future work, while every revision must remain complete.

Changing block 2 toX stores/transfers X plus metadata; reconstructing versionB does not replayA. Shared blocks remain live while B retains them even if A is retired. The engine does not parse Unreal objects. Compression/encryption or whole-asset rewrites can change most bytes and reduce deduplication.

```text
Version A : [bloc 1][bloc 2][bloc 3][bloc 4]
Version B : [bloc 1][bloc X][bloc 3][bloc 4]
```


## Chunking policy {#politique-de-decoupage}

FastCDC2020 defaults:256 KiB minimum,1 MiB target,4 MiB maximum; fixed sizes also supported. CDC can recover common boundaries after insertions/deletions; average is not guaranteed. File-type policy and real-corpus tuning remain future work.

[Lore’s FastCDC choice](https://epicgames.github.io/lore/developing/decisions/00001-fast-cdc/) is a reference. [Syncthing BEP](https://docs.syncthing.net/specs/bep-v1.html) uses fixed 128 KiB..16 MiB blocks except the tail. Fixed chunking alone cannot make Syrnck objects/network compatible: hashes, metadata, identities, versions and messages differ. The separate [BEP adapter](doc:syncthing) bridges regular file blocks.

## Git mode {#mode-git}

Text remains ordinary Git blobs. Selected `.gitattributes` assets pass required clean/smudge filters into Syrnck pointers/reconstructed bytes. Failure cannot masquerade as a hydrated asset. The pointer differs from [Git LFS](https://raw.githubusercontent.com/git-lfs/git-lfs/main/docs/spec.md); GitHub/GitLab pointer hosting does not provide Syrnck block service/previews/LFS compatibility.

Offline [snapshot LFS migration](doc:git-lfs-migration) verifies SHA-256 data and builds only changed trees under a retained complete catalog before prepared Git ref publication. HEAD/index/workspace stay unchanged; [full history migration](doc:git-lfs-history) separately converts ancestry to new refs. Pending symbolic refs are checked under the Git lock.

Complete filtered manifests are pinned; canonical unhydrated pointers survive without incomplete pins for pointer-only clones. [Retention reconciliation](doc:git-retention) inventories refs/reflogs/stashes/all worktree indexes, verifies complete files and adds missing pins atomically without deleting unobserved pins. [Catalogs](doc:git-assets) bind a parentless revision to each commit, retained locally and immutable remotely. Git-net verifies catalogs/pointers around transfer. Opt-in pre-push sends complete ancestry before Git publication but is bypassable and not a distributed transaction.

Checkout policies local/download/skip select hydration. Download uses pointer manifest, read lease and persistent sessions independently of HEAD, verifying size/blocks/complete hash before output. [Filter-process](doc:git-filter-process) shares Store/cache across sequential requests without delayed hydration.

## Standalone mode {#mode-autonome}

Native history provides complete commits, frozen staging, branches/tags, first-parent log, new-directory export and archived history detachment. [Status/diff](doc:native-comparison) inspect without importing. [Staging](doc:native-staging) retains bytes/base; commit atomically moves head and consumes index, refusing stale bases.

[Text patches](doc:text-patches), [three-way text merge](doc:text-merge), [native merge](doc:native-merge), [linear/interactive rebase](doc:native-rebase), [cherry-pick/revert](doc:native-replay), [stash](doc:native-stash), [blame/exact rename history](doc:native-file-history) and [bisect](doc:native-bisect) exist with bounded policies. Optional [authored/signed revisions](doc:native-authorship) and [edited-rename alignment](doc:native-file-history#edited-renames) are available. Merge-preserving rebase remains future work.

[Workspace preflight](doc:native-workspace) separates base/index/work and content/mode/representation, requires clean tracked files for checkout, and includes ignored paths for collisions. Links/protected or ambiguous portable paths fail. Clean case/file-directory changes use the [namespace executor](doc:native-checkout#recoverable-namespace-transitions). Optional plan verification does not replace execution validation.

Recoverable checkout verifies source/target closures, retains a canonical-root-bound journal and prepares whole files before exclusive moves. Source backups survive until atomic branch/archive/target publication. Resume/abort is explicit; after publication only cleanup can resume. Workspace moves are sequential without external-editor lock; private bytes stay plaintext even with encrypted objects. GC/native writes block during active recovery; receipts/archives remain until explicit retirement.

HEAD is a chunk at `workspace/HEAD`, binding directory/selection; absence means main. Named descriptors keep v1; [detached HEAD](doc:native-detached) uses v2 and private revision/index roots. Switch journalv2 (namespacev4), or v7 for detached targets, guards old/new HEAD and both tips and publishes selection with the committed journal, preserving tips. CLI defaults and `Store::native_target()` resolve the selection. [Linked workspaces](doc:native-workspaces) scope HEAD/index/cache/journal and receipts under real `workspaces/NAME/` roots, share objects/named branches, and enforce branch reservations. Linked receipts use v8 with recorded context and layout; backups retain portable selections rather than absolute paths. Static partial workspaces retain complete logical revisions/indexes and project physical files with a canonical policy. Their v9 checkout receipts freeze both policies; dynamic selections and workspace administration remain incomplete.

Tags use target and optional annotation refs published atomically. Expected-tip deletion archives target/annotation/receipt and restore preserves archive. Pages bound JSON to 16 MiB. Selectors reject ambiguous branch/tag names. Signatures, server native branch policy and atomic multi-root transfer remain future work. Text storage may share chunks without exposing block-level differences in the user interface.

## Physical storage and packs {#stockage-physique-et-packs}

Logicalv1 objects can be loose or in indexed immutable packs. Pack appends new loose-only data; compact regroups. Explicit maintenance selects physicalv2/v3, never opening alone. All outputs are written/readback-verified before atomic config publication and removal. GC preserves fully live packs, drops dead ones and rewrites partial survivors.

Indexes use 256-record pages, validated fences/digests, negative-key filters and independent 4 MiB page LRU. Opening still verifies whole indexes and duplicate keys. Limits 1,048,576 entries/pack,1024 packs,4,194,304 total. GC/fsck stream packed marks; loose keys/stack/JSON still consume proportional memory. Public objects/closure materialize collections. Compaction merges pages and sorts loose keys only. Read sessions reuse at most eight LRU pack handles and close before publication/removal. Maintenance is exclusive; every envelope remains verified, including both copies in fsck. See [packs](doc:packs), [indexes](doc:indexes), [audit](doc:audit), [compaction](doc:compaction).

## Ingest pipeline {#pipeline-d-ingestion}

Optional 2..64 workers encode new chunks; default is sequential. Coordinator owns CDC, hashes, deduplication, manifest order and atomic writes. Workers own reusable Zstd contexts and never mutate Store/refs. Raw byte budget and 2*workers pending-task cap provide backpressure.

Raw inputs are released before encoded results return. Encoded buffers/read buffers/codecs/metadata add memory beyond that budget. Flat serialized manifests stop at 16 MiB during reading, which does not bound JSON allocations. First-occurrence block order determines publication; manifest waits for every task. Errors/cancellation stop and join workers; existing refs stay intact, already published orphan chunks may remain for retry/GC. Internal workers do not make a Store concurrently callable.

## Cache and validation traversal {#cache-et-parcours-de-validation}

Handle-local cache is never disk authority. Entries require authenticated decrypt, bounded decompress, ID/format validation and bind typed keys to exact envelope BLAKE3. Every later access rereads/hashes the envelope before reuse; changed bytes evict and fully validate, missing data stays missing.

Independent LRUs default 8192 stamps/64 MiB decoded bytes, excluding indexes/work buffers. Stamps can survive decoded eviction. Cache retains nothing through GC, never persists across open and clears decoded buffers before release. Authorized-process plaintext remains distinct from at-rest encryption.

Closure validation hashes each full file while traversing ordered blocks and records dependencies including parents. Shared blocks may be reread for distinct full-file hashes. Fsck streams all stored data; GC validates retained roots before deletion. Both bypass caches and restore policy even after failure.

## Retention and removal {#conservation-et-suppression}

Branches, tags, indexes, archives, pins and received references retain transitive dependencies including parents. Exclusive Store locking prevents other writers during GC; missing/corrupt retained objects stop deletion. Three separate actions: remove a commit from a branch, release retention roots, physically collect unreachable objects. Detachment archives the old head first; use [backup-protected retirement](doc:archive-retention) to release archives. Fully unretained collected versions cannot be reconstructed.

[Transfer journals](doc:transfer-sessions), [upload leases](doc:server-upload-leases) and read leases protect in-progress work with selective retention/inventories. Releases, verified backups and grace policies need deliberate administration; distributed GC cannot rely on one client’s roots.

## Synchronization and server {#synchronisation-et-serveur}

Agent compares typed IDs, asks what is missing, transfers verified objects and publishes only a complete closure with expected-value CAS. One Store server supports shared-token or [configured identities/permissions](doc:server-access), serializes storage and excludes offline GC/other Store processes while running. Logical objects are at most 16 MiB, missing batches 4096, server pool 16 threads. [Parallel transfer](doc:parallel-transfers) bounds requests/buffers while Store access stays coordinated.

Block transfer does not resolve concurrent edits. The [resident native agent](doc:resident-agent) exchanges guarded revisions and can merge compatible histories. The [BEP controller](doc:syncthing) retains vectors/tombstones and can preserve concurrent regular files. Multi-server publication, directory/symlink metadata and distributed retention remain unfinished. [Read leases](doc:server-read-leases) retain exact roots even after source-ref removal and renew independently, releasing after local completion. [Inventories](doc:upload-inventories) declare keys before presence checks for selective GC; old leases retain global protection. [Receipt pruning](doc:selective-retention) is explicit. Distributed GC remains future work.

C/Go expose object IO, closures, pins and workspace observation. The [BEP adapter](doc:syncthing) uses the upstream Syncthing library’s fixed SHA-256 file blocks without changing Syrnck storage; it is not a full daemon or CDC extension between Syncthing peers.

## Encryption {#chiffrement}

Objects are compressed then encrypted at rest with XChaCha20-Poly1305, random 256-bit key/192-bit nonce, typed ID authenticated as associated data. Manifests/revisions are encrypted; config, refs, IDs and stored sizes remain visible. Key is supplied through SYRNCK_KEY, not config. No password derivation, in-place rotation, recovery or OS vault. Process/environment owners are trusted. Backups can reencrypt to another key.

[Network Zstd](doc:network-compression) level 1 negotiates logical frames, falling back to raw for poor gain/budget. Decoder validates before import; network counters/throttling include retries separately from successful logical bytes.

Transfers expose decoded objects through HTTPS to a trusted server. End-to-end encryption would need opaque metadata, dedup/equality-leakage policy, rollback protection and device revocation. [Syncthing untrusted encryption](https://docs.syncthing.net/specs/untrusted.html) binds blocks to file/offset, reducing correlation but preventing the desired cross-file/position deduplication if adopted unchanged. Its TLS certificate/device identity is distinct from Syrnck token/proxy transport.

## Integrity and current limits {#integrite-et-limites-actuelles}

Objects use temporary files and atomic publication; refs publish after dependency verification. Reconstruction checks every chunk/complete hash and refuses overwrite. Directory export prepares a complete tree and publishes exclusively to a new path; kill may leave staging. Checkout/switch in existing workspaces journal sequential moves, not a globally atomic tree.

Scanner excludes Git/Store/cache and ignore rules, refuses links/special files, requires portable UTF-8 paths and uses utf8proc normalization/casefold for collisions. ACLs,xattrs,hardlinks,empty directories are not stored. Unix executable bits are supported; Windows keeps snapshot metadata. Size/mtime checks around reads do not provide a filesystem snapshot: stabilize files during commit. Power-loss, network shares and writers bypassing locks need further validation. Original code is MIT; [dependency notices](doc:licenses) retain their own terms.
