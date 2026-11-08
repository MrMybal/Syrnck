# Syrnck 0.1.0 — source version notes

[English](VERSION.md) · [Français](VERSION.fr.md) · [README](README.md)

## Status

**Experimental prototype.** The version `0.1.0` is declared by the CMake project and CLI. These notes describe the current source checkout, not a published binary release. No application release or installer is currently listed on the [release page](https://github.com/MrMybal/Syrnck/releases).

Legacy objects v1, optional authored revisions v2, physical store formats v1/v2/v3 and C ABI 1 are separate version numbers. ABI 1 has additive entry points: pair bindings and headers with a matching native library.

## Changes in the current checkout

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
- Exact-content rename history exists; edited renames/copies and their merge/rebase integration remain incomplete. Authored revisions are opt-in; server access v2 can require approved signing keys. Registration is manual and policy changes require restart. OS signing-key vaults, distributed revocation and merge-preserving rebase remain unfinished.
- `agent run` continuously captures selected local changes, deletions and exact renames, retains offline snapshots and resumes guarded native publication. The optional `--bidirectional` mode applies incoming descendants and merges compatible concurrent histories with both parents preserved. Unresolved conflicts and acknowledged remote deletion/rewinds are blocked. The agent can recover its own guarded transactions offline and retains observed remote history before completing publications. Automatic conflict copies and OS service installation remain unfinished.
- The optional Syncthing adapter covers regular-file BEP data. Device discovery, pairing, version vectors, conflicts and full daemon/folder synchronization are outside that adapter.
- XChaCha20-Poly1305 protects objects at rest. The trusted server receives decoded objects. End-to-end encryption, key recovery and distributed rotation/revocation remain unfinished. Checkout/staging files can contain plaintext.
- Flat manifests and revision metadata are limited to 16 MiB. Cache and in-flight budgets do not bound total process memory. No real Unreal-project throughput guarantee is established.
- Archives and other roots retain old versions until explicit retirement. Removing all roots and collecting data makes those versions unrecoverable without a retained backup. Automatic retention policy and distributed GC remain unfinished.
- Process-interruption recovery does not establish power-loss or network-filesystem durability. Keep working files stable during scans and external writers stopped during checkout.

See [the complete current limits](Documentation/Documentation.html#/en/limits) and [format reference](Documentation/Documentation.html#/en/protocol).

## Platforms

The current development build is Windows x64. CMake/CI configurations exist for Windows, Linux and macOS, with a separate Unix sanitizer configuration; those configurations do not establish that a released binary is available or validated on each target. Linux/macOS execution and other architectures still need target validation.

The core Go binding has Windows DLL and Unix cgo backends. The optional Syncthing module has separate requirements. Neither backend declaration establishes ARM64 validation.

## Bugs and feedback

These notes do not maintain a ticket-by-ticket bug list. Consult the [issue tracker](https://github.com/MrMybal/Syrnck/issues) for reports and ongoing investigations. Describe the command, expected behavior, actual error, platform and relevant version when reporting a reproducible issue.

## References

- [README and source installation](README.md#installation).
- [Local manual](Documentation/Documentation.html#/en/overview).
- [Repository](https://github.com/MrMybal/Syrnck) · [Releases](https://github.com/MrMybal/Syrnck/releases).
- [Application license](LICENSE) · [Third-party notices](THIRD_PARTY.md).
