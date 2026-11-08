---
id: signing-api
title: Ed25519 signing API
description: Move-only in-memory signing keys, standard signatures and strict verification.
section: API
order: 650
parent: integrations
revision: 3
---

## Sign and verify bytes {#usage}

```cpp
#include <syrnck/signing.hpp>
auto signer = syrnck::SigningKey::generate();
auto public_key = signer.public_key();
auto signature = signer.sign(payload);
bool valid = syrnck::verify_ed25519(public_key, signature, payload);
auto fingerprint = syrnck::signing_key_id(public_key);
```

`payload` is a span of bytes. Empty messages are supported. Public keys contain
32 bytes; signatures contain 64 bytes. This is Ed25519 with SHA-512 as specified
in [RFC 8032](https://www.rfc-editor.org/rfc/rfc8032), implemented by the optional
module of vendored Monocypher 4.0.3. The base library's BLAKE2b EdDSA variant is
a different algorithm. No OpenSSL or network runtime is required by this API.

## Key ownership {#ownership}

`SigningKey::from_seed(bytes)` accepts exactly 32 bytes, copies the caller's
seed and expands it privately. The caller retains ownership of its input.
`generate()` uses the operating system's random source. The key object is
move-only, has no secret export API and wipes its expanded secret on destruction.
Moved-from objects refuse signing and public-key access. `public_key()` returns
a public copy. Keep signing seeds independent from encryption keys.

The fingerprint is `ed25519:` followed by BLAKE3 of the public key with the
`Syrnck Ed25519 public key v1:` domain prefix. It is a key identifier, not proof
that a person owns the key. The caller must authorize a public key independently
before attributing a signed payload to an identity.

## Verification contract {#verification}

Verification returns false for incorrect lengths, altered messages/signatures,
noncanonical point encodings and small-order public keys. The latter cannot
authenticate a signer. The upstream curve-equation primitive checks that case;
the engine does not implement its own elliptic-curve arithmetic. Encodings also
reject field elements outside the canonical range and negative-zero points.
Upstream verification checks the signature scalar and curve equation.

This verifies bytes supplied by the caller; applications must define their
signed payload and domain. The API does not establish identity trust, a trusted
timestamp, signature uniqueness or certificate validity. [Native authorship and commit signing](doc:native-authorship) use the versioned
revision payload. [Configured servers](doc:server-signatures) can authorize keys
and committer profiles. OS key-vault integration remains separate work. Legacy revision encodings remain unchanged.

## Build and verification {#validation}

```text
python scripts/build.py build --target syrnck_signing_tests
python scripts/build.py test -R "^signing$"
```

Tests cover three RFC 8032 vectors, every single-bit signature mutation in those
vectors, changed messages, malformed lengths, weak keys, noncanonical encodings
and key moves. They also construct a noncanonical negative-zero signature that
satisfies the upstream equation and require strict engine rejection. These are
functional checks, not a measurement of hardware side channels. Monocypher 4.0.3
contains the [upstream signing timing fix](https://monocypher.org/changelog).
Release provenance and checksums are recorded in
`third_party/monocypher/UPSTREAM.json`; dependency notices accompany distribution.
