---
id: usage
title: Getting started
description: Getting started guide.
section: Getting started
order: 5
revision: 1
---

# Getting started walkthrough {#parcours-du-prototype}

Examples assume `syrnck` is on PATH. In PowerShell use `&` with its absolute path if needed. Reuse IDs printed by commands without the example angle brackets. Open contextual help with `syrnck help` or `syrnck help native-tags`.

## Store and reconstruct a file {#stocker-et-reconstruire-un-fichier}

`init` creates a Store; `put` retains the returned manifest as `files/ID`. Reingest reports new/reused logical bytes; `get` refuses an existing destination. `diff` compares stored manifests. Reusable logical bytes do not equal network traffic: the receiver may already have other objects and repeated blocks.

`--workers` accepts 1..64, default 1; `--buffer-mib` defaults 32 MiB, must fit a maximum chunk, maximum 1024 MiB. Pending tasks also cap at 2*workers. Read buffers, encoded results, codecs and metadata add memory beyond this raw-byte budget. In-flight peaks are zero on the sequential path. IDs/boundaries stay identical; existing blocks are verified without recompression and pending duplicates encoded once. Gains depend on new data/storage/encryption.

C++ uses `IngestOptions` as third ingest argument and cooperative stop_token between reads/blocks/publications. Workers finish/join before return. Error/cancellation before finalization prevents manifest creation; low-level ingest changes no refs, though orphan chunks may remain for retry/GC. CLI interruption is connected. See [preferences](doc:settings) and [progress](doc:operations). Flat manifests are bounded 16 MiB during reading; paged metadata remains future work.

```text
syrnck init
syrnck put Content/Map.uasset
syrnck get <identifiant-manifeste> copie.uasset
syrnck inspect manifest <identifiant-manifeste>
syrnck fsck
```


```text
syrnck diff <ancien-manifeste> <nouveau-manifeste>
```


```text
syrnck put gros-fichier.bin --workers 4 --buffer-mib 32
```


## Verified-read cache {#cache-de-lectures-verifiees}

Defaults:64 MiB decoded bytes and 8192 verified stamps, independently bounded. Evicting bytes may keep a stamp for presence checks. Every access rereads and BLAKE3-hashes the entire envelope; only exact bytes previously authenticated/decoded/verified in this handle can reuse validation. The cache reduces codec work, not envelope IO, and does not rely on mtime/size.

Global `--cache-mib` accepts 0..1024 MiB;0 disables object caching. Indexes, IO/codecs/pipeline add memory. Cache vanishes with the handle/process; repeated reads/shared blocks in a server benefit most. C++ `StoreOptions` is constructor argument 4: `decoded_cache_bytes=0` keeps stamps, `verified_cache_entries=0` disables object cache. `io_stats()` reports reads/bytes/decodes/hits; reset keeps caches, clear releases object/page caches without removing data/refs. Decoded buffers are cleared on eviction/close.

`fsck`/`gc` bypass caches for current objects/dependencies/file hashes. Closure traversal hashes each manifest while reading its blocks; repeated positions still contribute each time. Index pages independently default 4 MiB, `--index-cache-mib`0..1024 or `pack_index_cache_bytes`. Directory/filter remain without cache; opening/audits verify full indexes, reread pages match validated snapshots. Index memory fields exclude overhead/temporaries. See [paged indexes](doc:indexes).

```text
syrnck --cache-mib 32 put fichier.bin
syrnck --cache-mib 0 get <manifeste> nouvelle-copie.bin
```


## Group objects into packs {#regrouper-les-objets-en-packs}

`pack` audits then appends packs for loose-only objects, leaving existing packs unchanged. Compact regroups all; defaults 512 MiB/65536 objects. IDs/refs/encryption envelopes stay intact. GC rewrites partly dead packs and drops wholly dead packs only after retained closure validation.

Streaming GC uses one bit per packed entry. `packed_mark_bytes`, `loose_live_objects`, `peak_pending_objects` report useful marks/keys/stack, not total RAM. [GC budgets](doc:packs#memoire-du-gc) include loose live keys/metadata.

Maintenance exclusively locks the Store; stop a server using it first. Explicit pack selects physicalv2/v3; opening does not convert, older binaries refuse unknown formats. Limits 1,048,576 objects/pack,1024 packs,4,194,304 packed objects. New packs need disk space before removing old ones. See [publication/recovery](doc:packs).

```text
syrnck pack
syrnck pack --compact
syrnck pack --max-mib 512 --max-objects 65536
syrnck gc
syrnck gc --delete
```


## Standalone history {#historique-autonome}

Adapt `examples/unreal.syrnckignore` into `.syrnckignore`. The Store must be inside the committed root, normally `.syrnck`. Native commands use the current branch, implicitly main in old Stores. Explicit `--branch` does not change selection. `head`, `switch-plan`, `switch` inspect/change it recoverably without moving tips. Ordinary commit scans the directory and refuses a staging index. Export requires a new destination with existing parent. Keep files stable while reading.

`add` freezes selected paths including deletions; `add .` selects all. `reset PATH...` resets selected index paths to base, while no-path reset clears the index even if stale; neither edits working files. Staging is retained through GC and consumed atomically with the branch commit. See [staging](doc:native-staging), [hunks](doc:native-hunks), [lines](doc:native-lines).

Status hashes without importing; revision/branch choose its base. Diff normally compares snapshots and has explicit [text options](doc:text-diff). Both produce JSON with progress/cancellation. Combined status separates base→index and index→work. Checkout-plan is read-only, detects ignored/untracked collisions and requires tracked clean/no index; verify audits the target closure. A blocked plan emits JSON/code 1.

Checkout moves the selected branch while archiving its old head. Resume/abort are alternatives after interruption. Sequential file moves are journaled; branch/archive/target publish together after verification. After publication only cleanup resumes. GC/native writes block while pending; prepared/backed-up bytes stay plaintext even with encrypted objects. Stop external writers. See [recovery](doc:native-checkout).

Tags name committed versions only, default HEAD, with optional annotation. Existing names fail. Expected-tip deletion atomically archives target/annotation/receipt; GC retains them. Restore preserves archive and refuses collisions. Selectors allow HEAD, unambiguous names, heads/NAME,tags/NAME; ambiguous names require explicit form. See [tags](doc:native-tags).

History detachment archives the old tip under `archive/ID` before moving the branch. Removing a commit is distinct from releasing roots and physical collection. Use [backup-protected retirement](doc:archive-retention) before dropping archives. Other roots still retain shared bytes; fully unretained collected versions cannot be reconstructed.

```text
syrnck native commit --message "Import du projet"
syrnck native commit --message "Modification de la carte"
syrnck native status --progress
syrnck native status --revision <identifiant-revision>
syrnck native status --combined
syrnck native checkout-plan <identifiant-revision> --verify
syrnck native diff <ancienne-revision> <nouvelle-revision>
syrnck native log
syrnck native branch release <identifiant-revision>
syrnck native commit --branch release --message "Préparation de la release"
syrnck native export <identifiant-revision> ../export-release
```


```text
syrnck native add Source Content/Maps --progress
syrnck native staged
syrnck native reset Source
syrnck native commit --staged --message "Cartes sélectionnées"
```


```text
syrnck native checkout <identifiant-revision> --branch main --progress
syrnck native checkout-status
syrnck native checkout-recover --progress
syrnck native checkout-recover --abort --progress
```


```text
syrnck native tag release/v1 HEAD -m "Version de livraison"
syrnck native tags --limit 100
syrnck native tag-show release/v1 --verify
syrnck native diff tags/release/v1 HEAD
syrnck native tag-delete release/v1 --expect <identifiant-revision>
syrnck native tag-archive <identifiant-archive> --verify
syrnck native tag-restore <identifiant-archive>
```


```text
syrnck native detach-history --message "Nouveau départ"
syrnck refs
syrnck gc
```


```text
syrnck refs
syrnck gc
```


## Git and assets {#git-et-assets}

Inside an existing Git repository, install the filters, then add selected asset patterns to `.gitattributes`. The Store is in the common Git directory under syrnck; installation prints its path. Configuration is repository-local with an absolute executable path, needing repair if moved. Source code remains ordinary Git blobs while assets become pointers in Git and full bytes in the workspace. Apply attributes to existing paths with targeted `git add --renormalize`, inspect before commit, and never stack LFS/Syrnck on the same path.

Offline `git lfs-preview HEAD`/`git migrate-lfs HEAD --branch converted-assets` verifies local SHA-256 LFS data, ingests blocks and prepares a child without changing HEAD/index/workspace. To convert ancestry use [full LFS history migration](doc:git-lfs-history). `git/ID` pins protect assets independently of Git reachability. Retention-scan inventories refs/reflogs/all registered worktree indexes without opening the Store; retention verifies and previews missing pins, apply adds atomically without deleting unobserved pins. See [retention](doc:git-retention).

Explicit agent transfers by manifest remain available. For normal workflows use [commit catalogs](doc:git-assets): configure remote URL and checkout download to fetch missing objects under a read lease, verify complete bytes before output; local remains default. Skip retains pointers and permits status/add without fetching; `SYRNCK_SKIP_SMUDGE=1` overrides without Store/key. A policy change does not rewrite existing files. To rehydrate an indexed pointer, preserve wanted edits, remove only that working pointer, then `git checkout -- PATH`.

`git catalog COMMIT` reads pointers without Store; push-assets/pull-assets transfer their verified closure. Opt-in install-hook sends all ancestors before Git publication and fails on missing old bytes/network errors. Tokens stay in environment. Existing hooks are not replaced; custom core.hooksPath needs manual integration. A local hook can be bypassed and does not replace server-side Git validation.

The [resident filter-process](doc:git-filter-process) shares one Store/cache across a Git command, holding its exclusive lock until Git closes it. Other operations fail quickly while held. Single clean/smudge remain available.

```text
syrnck git install
```


```gitattributes
*.uasset filter=syrnck -diff -merge -text
*.umap filter=syrnck -diff -merge -text
```


```text
git add .gitattributes Content/Map.uasset Source/Game.cpp
git commit -m "Carte et code"
git switch -c feature
```


```text
syrnck --store <store-git> refs
syrnck --store <store-git> agent push https://blocs.exemple.test git/<manifeste> asset-release
```


```text
syrnck git install
syrnck --store <store-git-destinataire> agent pull https://blocs.exemple.test asset-release git/<manifeste>
```


## Server and agent {#serveur-et-agent}

Create a separate server Store. Configure [users/permissions](doc:server-access) or use the legacy shared token, at least 32 characters in SYRNCK_TOKEN on authorized peers. Avoid tokens in URLs/command lines. Server listens loopback; use a TLS proxy for remote access. Clients refuse non-loopback HTTP/redirects and accept a private CA with --ca. Deployment/certificate automation is not provided.

Agent push/pull are one-shot, without disk watching/conflict resolution. Retry reuses verified objects; incomplete refs never publish. The running server owns the exclusive Store lock and does no online GC. Persistent local sessions protect complete sources or partial received objects; resume uses recorded root/expected destination even after source-ref removal. Forget releases protection; prune previews completed receipts, --delete removes only verified completed journals, never incomplete sessions or data. Local administration needs no server token; uncertain publication is checked before retrying CAS.

Transfers support request/buffer bounds, retries 0..8(default 3), shared upload/download KiB/s with 0 unlimited. Waits cancel; final ref publication is not automatically retried. Compression auto-negotiates; off uses raw, zstd requires support if missing objects. Small/incompressible data may stay raw. Logical successful bytes differ from actual HTTP attempt bytes; throttling counts network bytes and compression budgets do not bound total RAM.

Upload leases/inventories negotiate automatically; require-lease enforces them and server lease-seconds configures lifetime. Offline leases list/release/prune administer them; new uploads declare paged inventories before presence queries for selective GC. Legacy leases without inventories still block GC globally. Expiry permits unretained collection and resume declares anew. Read leases protect exact roots through pull and are released once all data is local, before local publication; require-read-lease enforces support. See [sessions](doc:transfer-sessions), [upload](doc:server-upload-leases), [read leases](doc:server-read-leases), [inventories](doc:upload-inventories) and [retention](doc:selective-retention).

```text
syrnck --store serveur-data init
syrnck --store serveur-data serve --listen 127.0.0.1:7437
```


```text
syrnck agent push http://127.0.0.1:7437 heads/main main
```


```text
syrnck agent pull http://127.0.0.1:7437 main heads/received
syrnck native log --branch received
syrnck fsck
```


## Encryption at rest {#chiffrement-au-repos}

The PowerShell example generates a key into SYRNCK_KEY; save it in a secret manager before closing. Reuse that key on every encrypted Store open. Plain stores must open without it. [Backups](doc:backups) can copy to another Store/key; in-place rotation, distributed revocation and key recovery remain future work.

Each Store can use its own key. Transfers send decoded objects over secure transport and destinations reencrypt them. This does not provide end-to-end encryption against the server.

```powershell
$env:SYRNCK_KEY = (& syrnck keygen)
syrnck --store donnees-chiffrees init --encrypted
syrnck --store donnees-chiffrees put Content/Map.uasset
```


## C interface {#interface-c}

Build `syrnck_ffi` and `syrnck_ffi_static` for dynamic/static libraries. Include `syrnck.h`, use UTF-8 paths and close handles. Status codes and thread-local last_error convey failures; serialize Store calls. ABI1 exposes verified object IO, closures, pins, file verification, ranges/comparison and [workspace sessions](doc:workspace-abi). Network client is C++. See [ownership](doc:api-c-abi) and [Go binding](doc:go-binding).


## Example Unreal project exclusions {#ignore-example}

Adapt this sample to your own project before creating commits. Excluded working files are not retained by a native snapshot.

```gitignore
# Review for your project; copy to .syrnckignore for native commits.
**/Binaries/
**/DerivedDataCache/
**/Intermediate/
**/Saved/
/.vs/
/.idea/
/.tools/
/target/
*.sln
*.suo
*.key
*.pem
# Retain Content, Config, Source, Plugins and the .uproject file.
# Some plugins need prebuilt Binaries: adjust the exclusions in that case.
```
