---
id: key-files
title: Password-protected storage keys
description: Create, verify and rewrap key files; unlock native Stores without exporting keys.
section: Storage
parent: storage
order: 55
revision: 2
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

## Remaining security work {#limits}

These providers unlock existing at-rest encryption. They do not implement end-to-end object transport, an OS key vault, distributed key rotation/revocation or a protected revision-signing-seed provider. Checkout and persistent download staging can contain plaintext. See [current security limits](doc:limits#security).
