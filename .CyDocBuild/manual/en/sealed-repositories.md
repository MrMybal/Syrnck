---
id: sealed-repositories
title: End-to-end sealed repositories
description: Separate client keys, opaque server snapshots, differential transfers and rollback checkpoints.
section: Storage
parent: storage
order: 57
revision: 3
---

## What the server can see {#threat-model}

The explicit `sealed` workflow encrypts original chunks, manifests and revisions before handing them to an ordinary Syrnck transfer server. The client keeps a plaintext Store and a separate ciphertext **wire Store**. The server never receives the sealed master key. Its own optional at-rest key is independent and cannot decrypt the original objects.

File contents, original paths, commit messages, root names and original object IDs are encrypted. The wire representation exposes a public channel, random/keyed opaque IDs, object sizes/counts, reused-object equality, update timing and carrier history. Signed carriers also expose their author/committer identities. Equal original typed objects share a keyed address within one master-key realm, including across its channels. Use independent keys for independent privacy domains. This is not traffic-analysis resistance or an externally audited cryptographic system.

Ordinary `agent`, native and Git transfers retain their existing decoded-object protocol. Selecting an at-rest key does not enable this workflow. The native resident agent opts in with `--sealed --wire PATH`; the Syncthing BEP service does not yet consume sealed snapshots.

## Create and unlock a separate key {#key-files}

Supply `SYRNCK_SEALED_KEY_PASSWORD` through your trusted launcher, then create a protected file in an existing private directory outside the project:

```text
syrnck sealed-key create private-keys/team.syreakey
syrnck sealed-key inspect private-keys/team.syreakey
syrnck sealed-key verify private-keys/team.syreakey
```

Set `SYRNCK_SEALED_KEY_FILE` to that absolute path and keep `SYRNCK_SEALED_KEY_PASSWORD` available to the client. Alternatively set `SYRNCK_SEALED_KEY` to an existing 32-byte key encoded as 64 hex characters. Both providers together, an empty provider or invalid/wrong credentials are refused. The CLI clears these variables from its own process environment after loading, and before unrelated commands can launch children. The launching process's environment is unchanged.

`sealed-key protect FILE` wraps the exact raw sealed key. `sealed-key rewrap SOURCE TARGET` uses the old password and `SYRNCK_NEW_SEALED_KEY_PASSWORD`, with a new destination. The master key and public realm remain unchanged. All five key-file actions accept the same applicable password-variable/KDF controls as [storage key files](doc:key-files). The 104-byte authenticated envelope has a distinct sealed-key purpose: storage-key and signing-seed envelopes are refused. Rewrapping does not revoke old key copies or rotate encrypted objects. Store backups do not automatically include external key files.

## Prepare, inspect and restore locally {#local}

Initialize the plaintext source and destination Stores normally. Choose a wire path outside all plaintext Stores, conventional `.syrnck` project directories and registered native workspaces. The CLI creates a missing wire Store for preparation/push/fetch and reopens an existing one. It uses no at-rest key for this cache; its object bodies are already sealed. The C++ API can use independently encrypted wire Stores.

```text
syrnck --store project/.syrnck sealed snapshot heads/main --wire private-cache/team-wire
syrnck --store project/.syrnck sealed inspect SEALED_ID --wire private-cache/team-wire
syrnck --store clone/.syrnck sealed open SEALED_ID --wire private-cache/team-wire --checkpoint sealed/checkpoints/manual
```

Paths above are illustrative: `private-cache` must not be inside either project. `snapshot` takes explicit local retention-reference names; inspect `refs` to select them. `--channel NAME` defaults to `main`. For an incremental preparation, add `--previous SEALED_ID` from the same key/channel. A persistent opaque Merkle index reuses unchanged ciphertext and updates affected index paths. Identical roots return the previous sealed ID without generating new nonces.

Inspection is read-only. Opening verifies original typed hashes and whole-file hashes, imports original objects, then atomically publishes immutable `sealed/roots/ID/NAME_HASH` retention roots and the selected checkpoint. Native branches, staging and workspace files are not replaced. Successful publication wins a stop request in its completion callback. IO/output-allocation failures can still follow publication; reload refs and establish the outcome before retrying. Read the private `roots` report, then use the normal native checkout/export or manifest reconstruction workflow explicitly. Reports contain original root names/IDs and belong on the client.

## Transfer through the ordinary server {#remote}

Configure [server identities and branch policies](doc:server-access) for a dedicated channel. Writers need access to the carrier `sealed` namespace; an ordinary plaintext `Content`/`Source` write ACL is insufficient. Set the public carrier branch to fast-forward, with no plaintext-path asset-lock policy. Optional [signature policies](doc:server-signatures) authenticate signed carriers/index revisions. Configure the client's normal native identity/signer before preparation/publication when signatures are required. Its public identity is exposed on the carrier.

```text
syrnck --store project/.syrnck sealed push https://blocks.example.invalid heads/main --wire private-cache/team-wire --require-lease
syrnck --store clone/.syrnck sealed fetch https://blocks.example.invalid --wire private-cache/clone-wire --require-read-lease
```

Supply the existing `SYRNCK_TOKEN`/CA configuration. Request concurrency, buffering, compression, pacing and retry controls are the normal [transfer options](doc:transfer-policy). A push first observes an exact public root, downloads/authenticates its sealed history, checks the accepted local checkpoint, prepares ciphertext against that root and publishes with the original CAS expectation. It does not refresh the expectation after a race.

By default, changed original revision roots must fast-forward. Removing/replacing a root or changing a non-revision root requires `--allow-rewrite`. Divergent revisions require a merge or that explicit flag. The flag does not bypass server carrier policies. A server cannot inspect or enforce hidden original path ACLs, asset locks, commit signatures or original ancestry; key holders must enforce their own plaintext policies. Plaintext asset-lock claims are refused by sealed transfers.

## Resume the exact operation {#resume}

Transfer sessions live in the **wire Store**:

```text
syrnck --store private-cache/team-wire agent sessions
syrnck --store project/.syrnck sealed resume https://blocks.example.invalid SESSION --wire private-cache/team-wire
```

Use the same client key, destination/source Store and channel. An unfinished publication blocks another fresh push, preventing new random envelopes from obscuring an uncertain outcome. Resume preserves the prepared root and exact expected remote reference. Proven publication is checkpointed before session completion; a completed historical receipt cannot rewind a newer accepted checkpoint. Interrupted downloads reuse verified ciphertext and finish the original import/checkpoint operation. Do not forget an uncertain session to manufacture a new expectation.

Before checkpoint publication, an interrupted import may leave valid unreferenced original objects. Failed authentication cannot publish a plaintext root. Prepared wire roots and active transfer sessions protect ciphertext from local GC. Ordinary GC still refuses certain active operations according to its existing rules.

## History and freshness {#history}

The automatic checkpoint name is `sealed/checkpoints/ENDPOINT_FINGERPRINT/CHANNEL`. Once a snapshot is accepted, later snapshots must descend from it through authenticated previous-root commitments. An old valid root, a fork or a missing previously accepted public channel is refused. This protects a retained client checkpoint, not a first clone with no known root. A server can withhold newer data, present different first histories to different clients or deny service. Trusted initial-root exchange and cross-device checkpoint gossip remain future work.

Explicit historical local restoration uses a separate checkpoint name, as in `sealed/checkpoints/historical`. It does not reset the automatic remote checkpoint. Previous carrier roots and the growing object index retain old encrypted versions, even after an original history rewrite. Open/import the historical snapshot before expecting its original roots in a newly cloned plaintext Store. All these bytes remain retained until an explicit future retirement policy removes their protecting roots. Current sealed commands do not compact history or rotate/revoke realm keys.

## Format and resource bounds {#format}

Objects use fresh random 24-byte XChaCha20-Poly1305 nonces. Keyed BLAKE3 derives independent address/object/root keys from a random master. A `SYRSEA01` envelope contains a 16-byte public realm, nonce, ciphertext and tag; associated data binds its header, object/root domain and opaque address/channel. Object plaintext binds its original kind and ID. Root plaintext commits private roots, previous carrier, index root and public carrier version/time/identity. Server-visible Merkle nodes use ordinary validated revisions; files carry ciphertext through ordinary chunking, including the overhead of a 16 MiB original object. Encryption is not deterministic or convergent.

| CLI bound | Default | Meaning |
| --- | --- | --- |
| `--sealed-max-objects` | 4194304 | Original closure entries |
| `--sealed-max-work` | 268435456 | Charged traversal/index work |
| `--sealed-cache-bytes` | 16777216 | Parsed index-node cache; oversized nodes are read uncached |
| `--sealed-new-index-bytes` | 67108864 | Charged new address entries before encryption |
| `--sealed-max-history` | 100000 | Accepted-checkpoint predecessor traversal |

Nodes have at most 256 leaf entries or 16 child links. These bounds do not represent total RSS. Preparation/verification still audits full original closures and file bytes; only ciphertext generation, Merkle changes and transferred object bodies are differential. Large-project performance and non-Windows sealed workflows still need qualification. The [C++ API](doc:api-sealed-repositories), [C ABI](doc:api-c-abi#sealed-repositories) and [Go binding](doc:go-binding#sealed-repositories) provide local preparation/inspection/import, with purpose-separated native master owners. Resumable network wrappers remain C++/CLI. Sealed BEP integration, OS vaults, device membership, distributed key rotation/revocation, automatic replica repair and distributed retention remain unfinished.

## Encrypted resident synchronization {#resident}

```text
syrnck --store project/.syrnck agent run https://blocks.example.invalid --directory project --sealed --wire private-cache/resident-wire --bidirectional --require-lease --require-read-lease
```

Use the separate master-key providers above and the same dedicated server-channel policy. `--remote-branch main` selects the public channel; `--sealed-root heads/main` is the stable private root label inside it, independent of the local branch name. Exactly one revision root with that label is required. A differently labelled or multi-root snapshot is refused before workspace application. The five sealed resource bounds also apply to `agent run --sealed`. Ordinary agent mode remains decoded-object transport.

The existing watcher, offline capture, partial/detached workspace, merge, conflict-copy, rename and owned-checkout recovery policies apply to the original revisions. The adapter authenticates/imports an incoming snapshot before handing its original root to that state machine. The server receives only ciphertext carriers. A plain agent refuses to apply a recognized sealed carrier to an empty workspace. Unchanged accepted polls authenticate the cached root control without re-auditing/downloading the complete closure. Changed snapshots still receive the full verification described above.

Publication uses the exact public carrier observed before a local merge; it never refreshes that expectation during publication. The wire journal keeps the physical root/session. The plaintext Store retains candidates under `sealed/agent-candidates/TRANSPORT/ID` before journal creation, then records the crypto checkpoint and logical observation before completing the wire journal. A lost reply is reconciled first even if local HEAD advanced. Surviving completed wire receipts can restore a missing initial local observation. A newer accepted checkpoint never rewinds for an older receipt. A valid old/forked carrier is blocked even if its original revision ID equals local HEAD.

Keep both Stores for recovery. The service identity binds the endpoint, master-key realm and private root label as well as the ordinary workspace/branch binding. The master is opened once at process startup and held by its native owner; changing/removing its file does not alter a running process. Restarting reloads credentials. Store locks are released between ticks. Offline owned-checkout recovery still precedes network operations, but sealed startup requires its master. Original candidate pins and historical imports remain retained; automatic retirement and realm-key rotation are unfinished.

C++ callers can create a short-lived `net::sealed_native_sync(wire, remote, key, channel, private_root, limits)` adapter and pass it to `NativeSync::tick`. The wire Store, Remote and key must outlive it. The C/Go local snapshot APIs do not expose this network adapter. Syncthing BEP remains a separate integration.
