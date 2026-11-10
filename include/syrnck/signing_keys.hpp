#pragma once
#include <syrnck/keys.hpp>
#include <syrnck/signing.hpp>
namespace syrnck {
/**
 * @doc api-signing-keys
 * @locale en
 * @title Protected signing-key API
 * @section API
 * @symbol syrnck::ProtectedSigningKey
 * Include `<syrnck/signing_keys.hpp>` and link `Syrnck::core`.
 * See the [CLI and service guide](doc:signing-keys).
 *
 * ## Ownership {#ownership}
 * `generate_protected_signing_key(password, costs)` returns an encrypted envelope and public key.
 * `protect_signing_seed(seed, password, costs)` protects exactly 32 caller-owned seed bytes.
 * `open_protected_signing_key(envelope, password)` authenticates and returns a move-only `SigningKey`.
 * The caller owns/wipes password and seed inputs. No plaintext seed export exists.
 * `SigningKey::clone()` explicitly creates an independent native signing owner; each owner wipes its material.
 *
 * ## Envelopes and passwords {#envelopes}
 * `protected_signing_key_options` inspects public costs only; it does not authenticate.
 * `change_signing_key_password(envelope, old, next, costs)` returns the same signer with fresh salt/nonce.
 * Costs default to authenticated input values. Old envelopes are not revoked.
 * The purpose-separated 104-byte format rejects storage envelopes. Costs and synchronous KDF limits
 * match the [password API](doc:api-password-keys#ownership). No OS vault or end-to-end encryption is provided.
 */
/**
 * @doc api-signing-keys
 * @locale fr
 * @title API des clés de signature protégées
 * @section API
 * @symbol syrnck::ProtectedSigningKey
 * Inclure `<syrnck/signing_keys.hpp>` et lier `Syrnck::core`.
 * Voir le [guide CLI et services](doc:signing-keys).
 *
 * ## Propriété {#ownership}
 * `generate_protected_signing_key(password, costs)` rend une enveloppe chiffrée et la clé publique.
 * `protect_signing_seed(seed, password, costs)` protège exactement 32 octets de graine de l’appelant.
 * `open_protected_signing_key(envelope, password)` authentifie et rend une `SigningKey` déplaçable.
 * L’appelant possède/efface mots de passe et graines. Aucun export de graine brute.
 * `SigningKey::clone()` crée explicitement un propriétaire natif indépendant ; chacun efface son matériel secret.
 *
 * ## Enveloppes et mots de passe {#envelopes}
 * `protected_signing_key_options` lit les coûts publics sans authentification.
 * `change_signing_key_password(envelope, old, next, costs)` rend le même signataire avec un sel/nonce neuf.
 * Les coûts précédents authentifiés sont conservés par défaut. Aucune révocation des anciennes enveloppes.
 * Le format dédié de 104 octets refuse les enveloppes de stockage. Coûts et limites KDF synchrones sont ceux
 * de l’[API de mot de passe](doc:api-password-keys#ownership). Aucun coffre OS ni chiffrement de bout en bout.
 */
// An encrypted 32-byte Ed25519 seed and its public identity. No plaintext seed
// is exported. Envelopes are purpose-separated from storage-key v1 envelopes.
struct ProtectedSigningKey {
  Bytes envelope;
  SigningPublicKey public_key;
};
ProtectedSigningKey protect_signing_seed(std::span<const std::uint8_t> seed,
                                         std::span<const std::uint8_t> password, PasswordKdfOptions = {});
ProtectedSigningKey generate_protected_signing_key(std::span<const std::uint8_t> password,
                                                   PasswordKdfOptions = {});
// Header/cost inspection does not authenticate the envelope or public identity.
PasswordKdfOptions protected_signing_key_options(std::span<const std::uint8_t> envelope);
SigningKey open_protected_signing_key(std::span<const std::uint8_t> envelope,
                                      std::span<const std::uint8_t> password);
// Fresh salt/nonce; keeps the same signer. Does not revoke older envelopes or
// change server authorization policies. Costs default to the authenticated file.
ProtectedSigningKey change_signing_key_password(std::span<const std::uint8_t> envelope,
                                                std::span<const std::uint8_t> old_password,
                                                std::span<const std::uint8_t> new_password,
                                                std::optional<PasswordKdfOptions> = {});
} // namespace syrnck
