---
id: signing-keys
title: Protected revision signing keys
description: Protect Ed25519 seeds, configure native signers and retain one authenticated service owner.
section: Security
parent: native-authorship
order: 80
revision: 2
---

## Create a protected signer {#create}

Set `SYRNCK_SIGNING_KEY_PASSWORD` through a trusted launcher and use an existing
private directory outside the working tree. Passwords are never CLI arguments or
command output. Windows text becomes exact UTF-8; passwords contain 1..4096 bytes
without normalization or trimming.

```text
syrnck signing-key create private-keys/author.syrsig
syrnck signing-key inspect private-keys/author.syrsig
syrnck signing-key verify private-keys/author.syrsig
```

`create` generates an Ed25519 seed and writes only a 104-byte encrypted envelope.
Its output includes authenticated public_key/key_id. `inspect` reports public
KDF/cipher metadata with `authenticated: false` and does not claim a public signer
identity. `verify` authenticates and reports the public key, never the seed.
`--memory-mib 64..256` and `--passes 3..10` configure Argon2id; defaults are 64 MiB
and three passes. `--password-env NAME` selects another uppercase environment
name. Output files are exclusive; Unix mode is 0600 and Windows inherits the
private directory's security rules.

## Protect an existing seed {#existing}

For an existing signer, set its exact 64-hex-character `SYRNCK_SIGNING_SEED` and
the password, then run `syrnck signing-key protect private-keys/author.syrsig`.
The public identity remains the same. A storage key is a different purpose:
`key` commands and storage-key providers reject signing envelopes, and signing
providers reject storage envelopes.

## Change the password {#password-change}

Set the old password and `SYRNCK_NEW_SIGNING_KEY_PASSWORD`, then write a new file:

```text
syrnck signing-key rewrap private-keys/author.syrsig private-keys/author-new.syrsig
```

`--new-password-env NAME` selects a different new-password variable. Fresh salt
and nonce preserve the exact signer and existing KDF costs unless overridden.
The old envelope remains usable: this does not revoke a key or change server
authorization. Back up the envelope and credentials appropriately; a forgotten
password cannot be recovered.

## Configure the CLI {#configure}

Set `SYRNCK_SIGNING_KEY_FILE` to the protected file and supply its password.
Keep `SYRNCK_SIGNING_SEED` absent. Set a native author profile, then capture:

```text
syrnck native identity --name "Project author" --email author@example.invalid
syrnck native commit -m "Signed change"
```

The file provider is mutually exclusive with the raw seed, including an explicitly
empty raw variable. Missing/empty credentials, non-regular/linked files, malformed
headers, wrong purpose or failed authentication block signing operations without
an unsigned fallback. Invalid configuration preserves existing Store history.
Providing a signer requires signed fresh revisions for that process's identity.
Profiles retain only public name/email/timezone; they do not persist the file
location, password, seed or mandatory-signing policy. Keep provider settings in
the trusted launcher, and use [server signature rules](doc:server-signatures) for
team-wide enforcement.

The CLI clears signing credential variables before unrelated commands can spawn
children. The resident native agent authenticates once at startup and carries
its move-only expanded signer between short Store sessions. A removed or changed
file/password takes effect on restart; restart with an unavailable configured
provider refuses to start. Existing prepared signed checkout targets remain
recoverable without recomputing their signature.

## Native and service owners {#services}

The C++ API uses the same purpose-separated format:

```cpp
#include <syrnck/signing_keys.hpp>
auto sealed = syrnck::generate_protected_signing_key(password);
auto signer = syrnck::open_protected_signing_key(sealed.envelope, password);
store.use_native_identity(identity, std::move(signer), true);
```

The additive ABI-1 functions `signing_key_protect/change_password/open/close`,
`signing_key_public_json` and `store_use_native_signer` provide an opaque native
owner. Size queries do not authenticate or generate keys. Owners expose only
public JSON. Serialize calls and close each C handle once.

Go's `ProtectSigningKey`, `ChangeSigningKeyPassword` and `OpenProtectedSigner`
share the format. `SigningKey` must not be copied; Close is explicit/idempotent.
`NativeIdentityOptions.Signer` is mutually exclusive with Seed. An authenticated
owner attaches independent expanded signing state to each Store; closing the
owner does not revoke already configured Stores. No plaintext seed is exported
to Go. Use matching updated headers, binding and native library.

```go
owner, err := syrnck.OpenProtectedSigner(envelope, password)
if err != nil { return err }
defer owner.Close()
return store.ConfigureNativeIdentity(identity, syrnck.NativeIdentityOptions{
    Signer: owner, RequireSignature: true,
})
```

The standalone BEP service accepts these members in its normal configuration:

```json
{
  "author": {"name": "Project author", "email": "author@example.invalid", "timezone_offset": 0},
  "signing_key_file": "../private-keys/author.syrsig",
  "signing_key_password_env": "SYRNCK_SIGNING_KEY_PASSWORD"
}
```

This is a fragment, not a complete service configuration. Paths resolve relative
to the configuration file. `signing_key_file` requires an author and excludes
`signing_key_env`. Its password variable must be separate from storage credentials.
The file must be a regular non-linked 104-byte envelope outside the workspace or
inside its protected Store. Authentication finishes before folder workers or TCP
listeners start. The service removes the password variable after authentication,
retains a native owner until workers stop and requires signed fresh revisions.
Changed credentials require restart.


The CLI also accepts `SYRNCK_SIGNING_KEY_VAULT`, and the standalone service accepts `signing_key_vault`, through the [system vault workflow](doc:system-vaults#services). These exclude raw/file signers, require an author and retain typed native owners until shutdown. They do not store TLS device private keys.

## Format and limits {#format}

The experimental v1 signing envelope uses magic bytes `SYRSIG 01 00`, followed by
the same costs/salt/nonce/encrypted 32-byte payload/tag layout as
[storage key envelopes](doc:api-password-keys#protection). The complete 56-byte
header, including its purpose, is authenticated. Replacing the purpose bytes
does not convert a valid envelope: authentication fails. Argon2id v1.3 uses one
lane; XChaCha20-Poly1305 uses a fresh 16-byte salt and 24-byte nonce per seal.

KDF calls are synchronous and do not support cancellation within Argon2id.
Native owners and mutable KDF work buffers are wiped; caller-owned password
buffers, environment strings and language-runtime copies remain the caller's
responsibility. No locked-memory, OS-vault, distributed revocation, key rotation
or end-to-end-encryption guarantee is implied.
