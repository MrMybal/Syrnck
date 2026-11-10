---
id: system-vaults
title: Use system key vaults
description: Create native vault keys, export protected recovery copies and configure CLI or resident services.
section: Storage
parent: key-files
order: 56
revision: 1
---

## Create and inspect keys {#commands}

The `vault` commands use the current OS account's [system provider](doc:key-files#system-vault). `provider` reports compiled support without opening the vault. An available provider still requires an authorized, unlocked account session. No failure selects a different key provider.

```text
syrnck vault provider
syrnck vault create storage private-keys/project.vault.json
syrnck vault create signing private-keys/author.vault.json
syrnck vault create sealed private-keys/team.vault.json
syrnck vault inspect private-keys/author.vault.json
```

Use an existing parent directory. Creation generates a fresh independent key and random reference; existing files are never replaced. `inspect` authenticates the exact entry without returning its secret. It returns the signing public key/key ID or sealed realm when applicable.

On macOS, `create` and `import` accept `--keychain ABSOLUTE_PATH` for an existing file keychain. The selected path is recorded in the reference file. The headless CLI and standalone BEP executable explicitly disable all Keychain interaction in their own processes when a vault is selected. They do not change the account's defaults or another application's policy. Unlocking the keychain and authorizing the consuming executable remain the account owner's responsibility. Trust granted to the CLI does not automatically authorize a different executable such as `syrnck-bep`.

## Public reference files {#references}

A reference file contains public local selectors, not a key or a portable backup:

```json
{
  "version": 1,
  "type": "system-vault-reference",
  "provider": "windows-credential-manager",
  "reference": "syrnck-vault:1:storage:0000000000000000000000000000000000000000000000000000000000000000"
}
```

The zero ID above is an illustrative placeholder. Use a generated reference. The exact schema permits only these four fields and optional string `keychain` for macOS. Duplicate/unknown members, unsupported versions, malformed references, linked/nonregular files and files over 16 KiB are refused. Inspection/export/erasure verify the recorded provider. They do not accept a keychain override. Standard-output JSON also includes result fields; use the file written by the command as the receipt.

## Select CLI providers {#providers}

Set each variable to the receipt's **`reference` string**, rather than its filename:

| Purpose | Vault selector | Other providers, mutually exclusive |
| --- | --- | --- |
| Store encryption | `SYRNCK_KEY_VAULT` | `SYRNCK_KEY`, `SYRNCK_KEY_FILE` |
| Native revision signing | `SYRNCK_SIGNING_KEY_VAULT` | `SYRNCK_SIGNING_SEED`, `SYRNCK_SIGNING_KEY_FILE` |
| Sealed transport master | `SYRNCK_SEALED_KEY_VAULT` | `SYRNCK_SEALED_KEY`, `SYRNCK_SEALED_KEY_FILE` |

`SYRNCK_VAULT_KEYCHAIN` optionally selects one absolute macOS file-keychain path for these environment providers. Other platforms refuse this option. An empty/malformed selector, wrong key purpose or simultaneous raw/file/vault selection fails. Use `init --encrypted` when selecting a storage key. A signing key still requires a [native author identity](doc:native-authorship); a sealed master remains independent of the at-rest storage key.

Resident CLI agents cache authenticated native storage/signing owners by exact reference and selected keychain, so short Store sessions do not reread the OS vault. Signing/sealed variables are removed from the CLI's own environment when consumed, or before unrelated commands can launch children. The launcher environment is unchanged. Restart to discard retained keys or change account/trust policy; entry deletion does not revoke existing owners.

## Configure standalone BEP {#services}

Use public encoded references in the ordinary service JSON. This fragment requires the remaining [service settings](doc:syncthing#standalone-service):

```json
{
  "key_vault": "syrnck-vault:1:storage:0000000000000000000000000000000000000000000000000000000000000000",
  "author": {"name": "Project author", "email": "author@example.invalid", "timezone_offset": 0},
  "signing_key_vault": "syrnck-vault:1:signing:1111111111111111111111111111111111111111111111111111111111111111"
}
```

Replace both example references. `key_vault` excludes `key_file` and a nonempty raw value in `key_env` (default `SYRNCK_KEY`). `signing_key_vault` requires `author` and excludes `signing_key_env`/`signing_key_file`. Password-variable settings apply only to protected files. Optional `vault_keychain` must be an absolute macOS path and requires at least one vault selector. Invalid roles/configurations fail before workers/listeners start. Authentication loads typed native owners before capture/network activity and keeps them until shutdown; raw key/seed bytes never enter Go. Fresh revisions require signatures when a signer is configured. Restart to change credentials.

Explicit empty/null selectors and case aliases for the new vault members are refused. The opaque receive-encrypted role can use a storage vault key but still refuses author/signing options. TLS device private keys remain PEM files. Syncthing folder passwords and native sealed masters are separate protocols/credentials.

## Back up, restore and erase {#recovery}

Set `SYRNCK_KEY_PASSWORD` through your trusted launcher, or select another uppercase ASCII name with `--password-env NAME`:

```text
syrnck vault export private-keys/project.vault.json private-keys/project.syrkey
syrnck vault import storage private-keys/project.syrkey private-keys/recovered.vault.json
syrnck vault inspect private-keys/recovered.vault.json
syrnck vault erase private-keys/project.vault.json
```

`export` writes the existing purpose-separated 104-byte password envelope; optional `--memory-mib 64..256` and `--passes 3..10` configure Argon2id. `import` authenticates it before creating a fresh reference on the destination account/provider. Storage/signing/sealed envelopes cannot be substituted for each other. Outputs must be new files. Verify independent backups before relying on an account vault; Store backups do not include it.

`erase` removes only the exact selected entry, returns `removed: false` when absent, and leaves the public receipt in place. It does not revoke active/cached keys, exported copies or server authorizations, rotate encrypted repository data, or retire sealed history.

OS writes and filesystem receipt publication are separate. A publication error can occur after the file becomes visible. The command retains the OS entry and reports its public encoded reference. Establish the outcome before retrying; reconstruct the strict public receipt with the reported reference, compiled provider and originally selected keychain to inspect/export/erase it explicitly. An OS write failure can also have an uncertain provider outcome. These operations do not form a cross-system transaction.

## Qualification {#qualification}

Default `key_vault_cli` tests validate commands, strict receipts, provider exclusivity and refusal before Store creation without account access. Explicit `SYRNCK_TEST_SYSTEM_VAULT=ON` adds CLI checks when the CLI is built. `SYRNCK_TEST_SYSTEM_VAULT_GO=ON` includes native Go owners; `SYRNCK_TEST_SYSTEM_VAULT_BEP=ON` adds the actual BEP startup/shutdown and signed offline capture path. Both Go probes use strict pointer checks. Linux uses an isolated session bus/keyring; macOS a disposable selected keychain; Windows only random test-owned entries. Missing/locked providers fail without a plaintext fallback. Library users still choose the explicit macOS process policy themselves.
