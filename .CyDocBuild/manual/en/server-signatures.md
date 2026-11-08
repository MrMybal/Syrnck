---
id: server-signatures
title: Authorized revision signatures
description: Bind native committer keys to server accounts and require signed branch history.
section: Transfers
order: 515
parent: server-access
revision: 1
---

## Configure approved keys {#configuration}

The optional access-file schema v2 adds signing authorization to the existing
[accounts, permissions and asset locks](doc:server-access). V1 remains supported
with its exact original fields and behavior. V2 has exactly the same three root
fields, but every principal must add `signing_keys` and every branch must add
`require_signatures`. An older server rejects this configuration.

```json
{
  "version": 2,
  "principals": [
    {
      "id": "alice",
      "token_hash": "REPLACE_WITH_TOKEN_HASH",
      "role": "writer",
      "write_paths": ["Source", "Content"],
      "branches": ["main"],
      "signing_keys": [
        {
          "public_key": "REPLACE_WITH_64_LOWERCASE_HEX_PUBLIC_KEY",
          "name": "Alice",
          "email": "alice@example.invalid"
        }
      ]
    }
  ],
  "branches": [
    {"name": "main", "fast_forward": true, "lock_paths": ["Content"], "require_signatures": true}
  ]
}
```

Replace the token placeholder using `token-hash` and obtain the public key from
`native identity` with the signing key loaded, or a verified revision's signature.
The file contains public keys, never signing seeds. Each binding has exactly
`public_key,name,email`: a canonical Ed25519 public key and the exact UTF-8
committer identity it authorizes. Name/email limits match
[native identities](doc:native-authorship#identity). Timezone and timestamp remain
claims, not proof of when a person signed.

Each principal accepts at most 32 keys. A key can appear only once across the
whole configuration, including other accounts. Malformed, noncanonical,
non-curve or small-order public keys are rejected at startup. Readers must have
an empty key list; writers/admins may have none when they only transport another
approved committer's work. Existing 1 MiB and account/branch bounds still apply.

```text
syrnck --store server-store serve --access access-v2.json --require-lease
syrnck help server-signatures
```

## Validate every newly admitted commit {#validation}

On a branch with `require_signatures: true`, every newly reachable revision must
be v2 with a valid Ed25519 signature. Its public key must be registered, and the
committer name/email must exactly match its binding. The signer's account must
currently have permission to write the target branch and every changed path in
that commit, including each merge-parent comparison. A correct tip cannot hide
an unsigned, unknown-key or mismatched intermediate commit or second merge parent.
Content that is added and removed before the tip is still checked.

The authenticated publisher independently needs branch/path permissions and
valid asset-lock claims. Publisher and signer may be different: a teammate can
transport approved history. Admin role provides no signing-policy bypass.
Existing fast-forward rules, complete-closure verification, final asset-lock
expiration check and expected-value reference publication all still apply.
Rejection leaves the published reference unchanged; uploaded objects may remain
under the normal transfer/GC rules.

Only ancestors of this branch's current published head are an admitted baseline.
Enabling the policy on an existing branch does not require rewriting its legacy
history, but newly introduced commits must satisfy it. An unrelated branch's
head does not admit that history here. Initial publication into an empty branch
therefore requires the complete introduced history to satisfy the policy.

A real forced rewind must also target a currently approved signed tip, even when
the tip is already in the baseline; it cannot downgrade to legacy, unsigned or
revoked-key metadata. An exact no-op retry of the already published head may
finish a transfer after key revocation. It introduces no revision. With
`require_signatures: false`, ordinary token/path/branch rules apply, while object
validation still checks any signature that is present.

## Rotate and revoke {#rotation}

Treat the access file as administrator-controlled. Changes take effect after a
server restart, just like account/token changes. To rotate, register the new
public key alongside the old binding, distribute the new private key through
your own protected mechanism, then remove the old public key when appropriate.
Removing a binding revokes future admission under that key. Removing an account
or its branch/path permissions also affects future signed publications.

Already admitted history remains readable and retains its original signature
bytes. A new approved child may extend that baseline after an old key is revoked.
Reintroducing old-key commits through newly admitted history is refused. Token
rotation and signing-key rotation are independent. The policy uses the current
startup configuration, not a revision's self-declared date. Requests already in
progress complete under that immutable configuration.

## Trust boundaries {#limits}

Authorization concerns the current committer and the signed complete revision.
An original author can differ or be null after a rewrite; its name remains a
historical claim. There is no trusted timestamp, historical certificate validity,
distributed revocation feed, live policy reload or OS signing-key vault here.
This does not add per-path read confidentiality or end-to-end encryption. The
server still receives decoded objects and needs the normal HTTPS deployment.

## Verification {#verification}

```text
python scripts/build.py build --target syrnck_server_signatures_tests syrnck
python scripts/build.py test -R "^server_(signatures|signatures_policy|access_policy)$"
```

Tests cover strict configuration, key/profile mismatches, every newly admitted
parent, delegated transport, independent signer/publisher ACLs, admitted legacy
baselines, revocation, rotation and forbidden rewinds. Authenticated HTTP tests
also transfer signed history through an encrypted Store to a reader without the
signing private key, restart with changed policies and audit retained objects.
