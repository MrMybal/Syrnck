---
id: key-files
title: Password-protected storage keys
description: Create, verify and rewrap key files; unlock native Stores without exporting keys.
section: Storage
parent: storage
order: 55
revision: 5
---

## Create a key file {#create}

Set `SYRNCK_KEY_PASSWORD` through your trusted launcher, then create a key file in an existing private directory outside the project. Passwords are never command-line arguments or command output. Windows environment text is converted to UTF-8; no normalization or whitespace trimming is performed. The native password limit is 1..4096 bytes.

```text
syrnck key create private-keys/project.syrkey
syrnck key verify private-keys/project.syrkey
syrnck key inspect private-keys/project.syrkey
```

`create` generates a random 32-byte storage key and writes only its authenticated 104-byte envelope. `inspect` reads public KDF/cipher metadata and reports `authenticated: false`; it does not check the password or ciphertext. `verify` authenticates and reports `authenticated: true`, without exporting the key.

The default Argon2id costs are 64 MiB and three passes. `create`, `protect` and `rewrap` accept `--memory-mib 64..256` and `--passes 3..10`. `--password-env NAME` selects another uppercase ASCII environment name. Choose the private parent directory's access rules appropriately; Unix files are created with mode 0600, while Windows uses filesystem security inherited/defaulted for that directory.

## Protect an existing raw key {#existing}

For an existing encrypted Store, keep its exact 64-hex-character `SYRNCK_KEY`, set `SYRNCK_KEY_PASSWORD`, then run:

```text
syrnck key protect private-keys/project.syrkey
```

This wraps the existing key; it does not change any Store objects or references. `key create` always generates a new key, so use `protect` when retaining access to an existing Store.

## Unlock the CLI and resident agent {#providers}

Set `SYRNCK_KEY_FILE` to the absolute envelope-file path and `SYRNCK_KEY_PASSWORD` to its password. Unset `SYRNCK_KEY`: configuring both providers is refused. Initialize a new encrypted Store explicitly:

```text
syrnck --store project/.syrnck init --encrypted
syrnck --store project/.syrnck fsck
```

The file provider applies to native, Git, server and resident-agent commands that open a Store. Missing/wrong credentials, malformed files and linked input files fail without falling back to plaintext. Input must be one regular, non-linked 104-byte envelope file. Resident processes reuse authenticated keys for identical envelope/password bytes; changed files or credentials require fresh authentication. Restart a process when changing its inherited environment.

The optional standalone `syrnck-bep` executable accepts `key_file` and `key_password_env` in its [local configuration](doc:syncthing#standalone-service). Relative key paths resolve against the configuration file. It authenticates once at startup and retains a native key owner until workers have stopped. Envelope/password changes take effect after restart. Applications using the Go service library can supply the same owner in their `OpenStore` callback.

## Change a password {#password-change}

Set the old `SYRNCK_KEY_PASSWORD` and the new `SYRNCK_NEW_KEY_PASSWORD`:

```text
syrnck key rewrap private-keys/project.syrkey private-keys/project-new.syrkey
```

The destination must be new. The original remains unchanged, unspecified costs are preserved, and fresh salt/nonce protect the same storage key. `--new-password-env NAME` selects another new-password variable. Update the provider path/password after verifying the new file. Existing destinations, including links, are never replaced. A publication error can occur after the file becomes visible; inspect/verify the destination before retrying.

Old envelopes remain usable with their old passwords. Rewrapping is not cryptographic revocation, Store-object rotation or recovery of a forgotten password. Retain another protected copy of the key according to your backup policy; [Store backups](doc:backups) do not automatically retain external key files.

## C, C++ and Go integration {#sdk}

The [C++ API](doc:api-password-keys) owns secret bytes through move-only `SecretKey`. The [C ABI](doc:api-c-abi#protected-keys) and [Go binding](doc:go-binding#protected-keys) offer opaque native owners: authenticate once, open short Store sessions, and close the owner explicitly. Closing an owner does not invalidate already opened Stores, which retain their own keys. Password input buffers remain caller-owned.

## System key vaults {#system-vault}

The [native C++ vault API](doc:api-system-key-vault) can keep independent storage keys, signing seeds and sealed master keys in the current OS account. It is separate from the password-file providers above. CLI environment selection and the C/Go owners do not yet select this provider.

```cpp
#include <syrnck/key_vault.hpp>

syrnck::SystemKeyVault vault;
auto reference = vault.generate(syrnck::VaultKeyPurpose::sealed);
auto master = vault.open_secret(reference);
// Keep reference.encode() as public local configuration.
// Export a protected backup before relying on this machine's account vault.
auto backup = vault.export_protected(reference, backup_password);
```

`generate`, `save` and `import_protected` create fresh entries with random 256-bit IDs. There is no entry enumeration, caller-selected target name or update operation. References use a versioned purpose and lowercase hexadecimal ID; the record binds the same purpose/ID to the secret. `open_signer` returns a native signing owner, with no raw seed export. `export_protected` uses the existing authenticated 104-byte purpose-specific password format. The caller owns its password buffers and must save the returned backup outside the workspace. A reference alone cannot restore a lost account or machine.

| Platform | Provider and requirements |
| --- | --- |
| Windows | Credential Manager generic credentials, current account on this host (`CRED_PERSIST_LOCAL_MACHINE`). A usable logon session is required; network-only/sandbox sessions can refuse access. No enterprise roaming is requested. |
| macOS | Keychain generic passwords. The default keychain is used for creation; an optional absolute constructor path selects an existing keychain without changing the default or search list. Operations request failure instead of authentication UI. Unlock/trust policy must be configured by the account owner. |
| Linux | libsecret and the account's Secret Service session bus. Writes require an existing unlocked default collection. Reads do not request unlocking; duplicate matching records are refused. Library calls share a 30-second cancellation deadline per backend operation. Provider policy can require a prompt during creation/deletion; a resident host must arrange an unlocked authorized session. |

Configure `SYRNCK_SYSTEM_KEY_VAULT=AUTO` (default), `ON` (required) or `OFF`. Linux builds use `libsecret-1` development files when present; an installed SDK built with this provider also requires libsecret development files. `SystemKeyVault::provider()` reports `windows-credential-manager`, `macos-keychain`, `linux-secret-service` or `unavailable`; it does not prove an account session is unlocked. A missing provider/session, locked vault, missing reference or malformed record fails explicitly, with no plaintext/file fallback. Provider storage, OS/library copies and account permissions remain subject to the OS. This is not hardware-backed key isolation or a guarantee that another process in the same account cannot read credentials. Windows' native write API replaces an exact existing target; Syrnck generates an unpredictable new ID and checks absence, rather than offering an atomic caller-selected create-if-absent contract.

Deleting an entry does not invalidate existing native owners, rotate repository encryption, revoke exported copies or remove a server authorization. Export/verify independent protected backups before account changes. Ordinary Store backups do not capture the OS vault. Opt-in real-provider qualification uses `SYRNCK_TEST_SYSTEM_VAULT=ON` and `key_vault_system`; Linux runs a separate session bus/keyring, macOS creates and deletes a disposable selected keychain, and Windows creates/removes only its random synthetic test entries.

## Remaining security work {#limits}

The password-file providers unlock existing at-rest encryption. The explicit [sealed workflow](doc:sealed-repositories) uses an independent master key and a distinct protected-key purpose for opaque transport. The separate C++ system vault does not yet integrate CLI/C/Go provider selection or distributed key rotation/revocation. Revision signing uses [separate protected signers](doc:signing-keys). Checkout and persistent download staging can contain plaintext. See [current security limits](doc:limits#security).
