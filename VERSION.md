# Syrnck 0.1.0 — source version notes

[English](VERSION.md) · [Français](VERSION.fr.md) · [README](README.md)

## Status

**Experimental prototype.** The version `0.1.0` is declared by the CMake project and CLI. These notes describe the current source checkout, not a published binary release. No application release or installer is currently listed on the [release page](https://github.com/MrMybal/Syrnck/releases).

Legacy objects v1, optional authored revisions v2, physical store formats v1/v2/v3 and C ABI 1 are separate version numbers. ABI 1 has additive entry points: pair bindings and headers with a matching native library.

## Changes in the current checkout

- Opaque encrypted BEP receiver with retained block receipts/history, paired resident sessions and a one-peer standalone role; real Syncthing upload, fresh restore and process-restart qualification.

- Explicit sealed C++/C/Go snapshots and resumable C++/CLI ciphertext transfers: independent protected keys, opaque metadata, persistent Merkle reuse and retained-client rollback checks. The native resident agent supports sealed transport; Syncthing uses its separate encrypted-device protocol.
- Native merge-preserving rebase with ordered DAG replay, recursive merge bases and frozen recovery; Windows Go syscall pointer lifetimes are regression-tested across GC and goroutine stack changes.
- Bounded edited-rename detection across native history/blame, merge/replay/rebase/index and signed resident synchronization, with explicit structural conflicts and frozen checkout recovery.
- Relocatable native CMake SDK with exported C++/C targets, bundled static dependencies and independent installed-consumer qualification.
- Password-protected storage keys and separate signing seeds: bounded Argon2id envelopes, exclusive CLI key files and password changes, reusable C/Go native owners and resident/standalone BEP providers.
- Native C++ system vault API for independent storage/signing/sealed keys, with protected backup export. Real Windows Release/Debug qualification; dedicated macOS/Linux provider qualification and CLI/C/Go provider selection remain pending.
- C/Go handle-local native authors/signers and verified fresh revision preparation; required signatures block unsigned fallback while retained signed candidates recover without a private key.
- Optional persistent BEP staging resumes verified ranges after connection/process loss, with an independent OS lease and completed-staging cleanup after catalog retention; plaintext staging requires explicit opt-in.
- Optional standalone `syrnck-bep` with strict local configuration, EN/FR help, explicit device pinning, offline capture, process-restart qualification and packaged runtime source/license notices.

- Native-backed BEP resident service API with independent offline capture, paired reconnection/backoff and joined transport/TLS shutdown; OS service installation remains unfinished.

- Session-local manifest reuse for resident-agent and C/Go captures, with strong filesystem stamps, complete inventory/object verification and separate source I/O counters.

- Explicit owned recovery preserves regular-file edits made during checkout through native archives and exclusive conflict copies, without rewriting signed incoming candidates.

- Guarded native workspace capture, checkout planning and owned recovery through C/Go, including exact revision/materialization expectations and Store/context binding.
- Native-backed Syncthing folder catalogs retain causal indexes, tombstones and complete file history atomically; staged downloads release the engine lock during network waits, with real-daemon reconnection qualification.
- Additive C/Go SDK object creation, unpinned memory/stream preparation and atomic typed reference groups for compound state retained through GC and backups.
- Native macOS NFD callback filenames normalize to canonical NFC for Syncthing indexes, block requests and reconstruction.
- Authenticated Syncthing BEP device connections and reproducible live upstream daemon qualification, including differential transfers into encrypted engine storage.
- Explicit deterministic conflict copies for native merges and bidirectional agents, preserving asset contents and signed recoverable candidates.
- C++20 block storage shared by Git assets and native revisions: complete manifests, reusable chunks, verified ranges, packs, backups and explicit retention.
- Native history and recoverable workspace workflows, plus an authenticated block server and resumable one-shot transfers.
- Detached native HEAD with a private index, recoverable switching and retained detached histories.
- Native authors/committers and optional Ed25519 signatures, with original authors preserved during rewrites and strict verification on object reads/imports.
- Native line attribution through merge parents and exact renames, with verified stored content and explicit input/work/output budgets.
- A local CyDoc manual in English and French, contextual `syrnck help`, public-header API annotations and documentation packaging in build/install/CI.
- English/French README editions with source installation, scoped dependency notices and these version notes.

These source features are not an announcement of a prebuilt release. Each operation’s exact behavior and limits are in the [manual](Documentation/Documentation.html#/en/overview).

## Documented limitations

- Named/detached HEAD, independent linked workspaces, recoverable live selection changes, partial hard reset/stash and archived unregister/restore are available. Linked relocation stays on the same filesystem; primary/cross-filesystem relocation remains unfinished.
- Exact and optional bounded edited renames cover native history/blame and merge/replay/rebase/index/agent workflows; copy/swap inference remains incomplete. Authored revisions are opt-in; server access v2 can require approved signing keys. Registration is manual and policy changes require restart. OS signing-key vaults and distributed revocation remain unfinished.
- `agent run` continuously captures selected local changes, deletions and exact renames, retains offline snapshots and resumes guarded native publication. The optional `--bidirectional` mode applies incoming descendants and merges compatible concurrent histories with both parents preserved. Unresolved conflicts and acknowledged remote deletion/rewinds are blocked. The agent can recover its own guarded transactions offline and retains observed remote history before completing publications. Explicit conflict copies preserve concurrent data; OS service installation remains unfinished.
- The optional Syncthing adapter covers regular-file BEP data and explicitly paired device connections. Its catalog persists folder/index state and reports causal conflicts. Its continuous controller applies regular files and tombstones to native workspaces, with receipt-backed recovery and real-daemon reconnection tests. Explicit conflict copies resolve concurrent regular-file vectors and retain both inputs. Directory/symlink metadata, OS service installation, discovery, a pairing UI, membership management and automatic history retirement remain incomplete.
- XChaCha20-Poly1305 protects objects at rest. Ordinary transfers give decoded objects to a trusted server; the separate sealed C++/CLI workflow keeps original objects/metadata encrypted on the wire server. Trusted first-root exchange, key recovery and distributed rotation/revocation remain unfinished. Checkout/staging files can contain plaintext.
- Flat manifests and revision metadata are limited to 16 MiB. Cache and in-flight budgets do not bound total process memory. No real Unreal-project throughput guarantee is established.
- Archives and other roots retain old versions until explicit retirement. Removing all roots and collecting data makes those versions unrecoverable without a retained backup. Automatic retention policy and distributed GC remain unfinished.
- Process-interruption recovery does not establish power-loss or network-filesystem durability. Keep working files stable during scans and external writers stopped during checkout.

See [the complete current limits](Documentation/Documentation.html#/en/limits) and [format reference](Documentation/Documentation.html#/en/protocol).

## Platforms

The local development build is Windows x64. Linux CI for [`ccc06a4`](https://github.com/MrMybal/Syrnck/actions/runs/42800607349) passed all 456 tests and distribution installation. The Windows/macOS/sanitizer jobs of that run were still running when the system-vault milestone was prepared. This evidence predates the new vault providers; their dedicated CI checks actual providers and provider-free SDKs separately. CMake configurations and individual successful jobs do not establish a published or fully qualified release. Other architectures, including ARM64, still require target validation.

The core Go binding has Windows DLL and Unix cgo backends. The optional Syncthing module has separate requirements. Neither backend declaration establishes ARM64 validation.

## Bugs and feedback

These notes do not maintain a ticket-by-ticket bug list. Consult the [issue tracker](https://github.com/MrMybal/Syrnck/issues) for reports and ongoing investigations. Describe the command, expected behavior, actual error, platform and relevant version when reporting a reproducible issue.

## References

- [README and source installation](README.md#installation).
- [Local manual](Documentation/Documentation.html#/en/overview).
- [Repository](https://github.com/MrMybal/Syrnck) · [Releases](https://github.com/MrMybal/Syrnck/releases).
- [Application license](LICENSE) · [Third-party notices](THIRD_PARTY.md).
