---
id: native-authorship
title: Native authors and signatures
description: Original authors, current committers and optional Ed25519 signatures.
section: Version control
order: 305
parent: native-history
revision: 4
---

## Configure an identity {#identity}

```text
syrnck native identity --name "Alice" --email "alice@example.invalid" --timezone-offset +60
syrnck native identity
syrnck native log --json
syrnck native log --show-signature
syrnck native identity --clear
```

Names and addresses are UTF-8 text, limited to 256 and 320 bytes, without control
characters. The timezone is minutes east of UTC, in -1439..1439; the default is 0.
Addresses are recorded literally, without proving mailbox ownership. JSON fields,
IDs and key fingerprints are identical in the English and French interfaces.

The optional machine-local `native-identity.json` inside the Store contains only
the version, type, name, email and timezone. It applies to all workspaces using
that Store. Logical backups do not copy this file. Clearing the profile preserves
existing revisions and the additive reader requirement.

Paired `SYRNCK_USER_NAME` and `SYRNCK_USER_EMAIL` override the saved profile for
one invocation; `SYRNCK_USER_TIMEZONE` supplies an optional offset. A newly created
revision uses legacy v1 when no identity is configured, including after an
explicit clear. Rewriting an authored v2 revision requires a current identity.

## Sign without storing a private key {#signing}

An optional `SYRNCK_SIGNING_SEED` supplies a 32-byte Ed25519 seed encoded as 64
hexadecimal characters. Supply it through a controlled process environment;
do not put a real seed in command arguments, scripts or a profile file. The CLI
consumes and removes the variable from its process environment and wipes its
owned temporary buffers. Unrelated commands discard it before spawning tools.
This does not promise erasure of earlier copies maintained by the OS or caller.
An identity without a seed creates an unsigned v2 revision with `signature: null`.

In the C++ library, identity and signing keys are explicit handle state:

```cpp
#include <syrnck/core.hpp>
store.use_native_identity({"Alice", "alice@example.invalid", 60},
                          syrnck::SigningKey::generate());
auto revision = store.commit_directory(project, "main", "Add asset");
store.clear_native_identity();
```

The Store owns the moved key exclusively. The library does not read the CLI
profile or environment automatically. `native_identity()` returns public profile
and key information; no private-key export is provided.
`take_native_identity()` detaches a move-only `NativeIdentityContext` containing
the profile and optional signing key. Move both into the next Store with
`use_native_identity()` before releasing the context. This supports short-lived
locked handles in a resident process without copying or exporting seed bytes.
The former handle becomes unconfigured; taking an empty context returns null.
Changing or taking identity inside a progress callback is refused. See the
[signing primitive](doc:signing-api) for seed ownership and strict verification.

## Author and committer rules {#rewrites}

A v2 revision records `author`, `committer` and an optional signature. Each person
stamp has `name,email,timestamp,timezone_offset`; the committer time equals the
revision timestamp. An original author may be null when rewriting legacy history.
The current committer signs the complete resulting revision, including its files
and parent IDs. An old signature is never copied onto changed metadata.

Fresh directory/staged commits, new merge commits, reverts and standalone squashes
use the current person as author and committer. Amend, cherry-pick, linear rebase
and history detachment preserve the original author and record the new committer.
Rebase squash/fixup steps preserve the first retained commit's original author.
An unchanged/fast-forward result retains the exact existing revision. Internal
index, stash and recovery snapshots remain v1; they do not invent authorship.

Rebase freezes one committer time for both candidate-calculation passes. With
an identity configured, `--committer-time SECONDS` on rebase-plan/rebase-todo/rebase
makes a preview reproducible with the same identity, key and inputs. Without
that option, separate invocations can choose different times and candidate IDs.
The report exposes `committer_timestamp`. Existing checkout recovery reads frozen
signed candidates and does not need the signing private key.

`native log --json`, file history, blame and archive summaries expose v2 authors,
committers and public signing-key IDs. Plain log shows the author and a different
committer; `--show-signature` adds the key ID or unsigned status. Legacy summary
fields and default first-parent log traversal remain compatible.

## Signed bytes and compatibility {#format}

Revision v2 is canonical compact JSON in the order
`version,parents,message,timestamp,author,committer,signature,files`.
The signature object contains exactly `algorithm,key_id,public_key,value`:
`ed25519`, the fingerprint, 32-byte public key and 64-byte signature; binary
values are lowercase hexadecimal. The signed message is ASCII
`Syrnck native revision signature v1:` followed by the 64-character lowercase
BLAKE3 hash of the canonical v2 revision with `signature` set to null.

Object import, reads and audits validate the signature, its fingerprint and the
canonical revision. Altering the author, parent, timestamp, message or file map
invalidates a signature. `authored-revisions-v1` is enabled before storing a v2
object or saving an identity profile. Historical readers reject the extended
Store configuration; envelope, manifest, pack, HTTP object and C ABI encodings
are unchanged. Logical backups/restores retain this reader requirement. V1 and
v2 revisions can coexist in the same graph.

## Trust and remaining limits {#trust}

A valid signature proves possession of its signing key for those bytes. A name,
email and timestamp remain claims by the signer. Server access v1 uses authenticated
principals, branch/path ACLs and asset locks; [access v2](doc:server-signatures)
can also require approved signatures and bind committer keys/identities to
accounts. It checks current policy for newly admitted history, preserving the
existing branch baseline. An OS signing-key vault and distributed revocation
remain separate work. [C and Go authorship APIs](doc:go-binding#native-authorship) configure handle-local
identities and prepare fresh signed revisions. A required-signature policy can
block new revisions without a seed while retaining public read/recovery access.
C++ `NativeIdentityContext` carries this policy when moving its exclusive signer
between Store handles. The policy and private key are not persisted.
