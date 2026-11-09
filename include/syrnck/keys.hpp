#pragma once
#include <syrnck/core.hpp>

namespace syrnck {
/**
 * @doc api-password-keys
 * @locale en
 * @title Password-protected key API
 * @section API
 * @symbol syrnck::SecretKey
 * Include `<syrnck/keys.hpp>` and link `Syrnck::core`. The [CLI key files](doc:key-files)
 * and [C/Go owners](doc:go-binding#protected-keys) use this same native envelope format.
 *
 * ## Ownership and derivation {#ownership}
 * `SecretKey::generate()` creates a random 32-byte secret. `from_bytes` copies exactly 32 bytes.
 * `from_password(password, salt, options)` uses Argon2id v1.3, one lane and 32 output bytes.
 * The salt must contain 16 bytes; generate a fresh salt for each independent derived key.
 * Default costs are 64 MiB and three passes; accepted costs are 64..256 whole MiB and 3..10 passes.
 * Passwords contain 1..4096 exact bytes, with no text normalization. Cost/size validation precedes allocation.
 *
 * Owners move without copying; moved-from access fails. Destruction wipes owned secret bytes;
 * KDF work memory is wiped before derivation returns.
 * `view()` borrows bytes until the owner is moved or destroyed; temporary owners cannot expose a view.
 * `wipe_secret` clears mutable caller-owned copies.
 * Callers retain responsibility for password buffers, copied `Key` values, file permissions and backups.
 * These functions are synchronous; they do not provide locked memory, an OS vault or cancellation inside Argon2.
 *
 * ## Protection and password changes {#protection}
 * `protect_secret_key(secret, password, options)` returns an authenticated 104-byte envelope.
 * `open_secret_key(envelope, password)` authenticates before returning secret bytes.
 * `protected_key_options(envelope)` reads validated public costs without authenticating the envelope.
 * `change_secret_key_password(envelope, oldPassword, newPassword, optionalCosts)` rewraps the same key,
 * preserving costs unless explicitly overridden. Every protection uses a fresh random salt and nonce.
 * An old envelope remains usable with its old password; changing the password does not rotate Store objects
 * or revoke other copies. A forgotten password requires another retained copy of the key.
 *
 * Version 1 bytes: `SYRKEY`, `01 00`, little-endian uint32 memory KiB and passes, 16-byte salt,
 * 24-byte nonce, 32-byte ciphertext, 16-byte tag. XChaCha20-Poly1305 authenticates the first 56 bytes
 * as associated data and encrypts only the secret. Headers, salt and nonce are public. Unknown sizes,
 * versions and out-of-range costs fail before derivation; wrong passwords or tampering fail authentication.
 *
 * This API does not save envelopes or change the Store format. Preserve envelopes and passwords outside Git
 * and working files. [Backup commands](doc:backups) retain their own destination-key selection contract.
 * See [storage](doc:storage) and [remaining security work](doc:limits#security).
 */
/**
 * @doc api-password-keys
 * @locale fr
 * @title API de protection des clés par mot de passe
 * @section API
 * @symbol syrnck::SecretKey
 * Inclure `<syrnck/keys.hpp>` et lier `Syrnck::core`. Les [fichiers de clés CLI](doc:key-files)
 * et les [propriétaires C/Go](doc:go-binding#protected-keys) utilisent ce même format natif d'enveloppe.
 *
 * ## Propriété et dérivation {#ownership}
 * `SecretKey::generate()` crée un secret aléatoire de 32 octets. `from_bytes` copie exactement 32 octets.
 * `from_password(password, salt, options)` utilise Argon2id v1.3, une voie et une sortie de 32 octets.
 * Le sel contient 16 octets ; générer un nouveau sel pour chaque clé dérivée indépendante.
 * Les coûts par défaut sont de 64 Mio et trois passes ; les valeurs admises vont de 64 à 256 Mio entiers et de 3 à 10 passes.
 * Les mots de passe contiennent de 1 à 4096 octets exacts, sans normalisation du texte. Les coûts et tailles sont validés avant allocation.
 *
 * Les propriétaires se déplacent sans copie ; l'accès à un objet déplacé échoue. La destruction efface les octets secrets détenus ;
 * la mémoire de dérivation est effacée avant le retour de la fonction.
 * `view()` emprunte les octets jusqu'au déplacement ou à la destruction du propriétaire ; les propriétaires temporaires ne peuvent pas exposer de vue.
 * `wipe_secret` efface les copies mutables détenues par l'appelant.
 * L'appelant reste responsable des buffers de mots de passe, des copies de `Key`, des permissions et des sauvegardes.
 * Ces fonctions sont synchrones ; elles ne fournissent ni mémoire verrouillée, ni coffre système, ni annulation pendant Argon2.
 *
 * ## Protection et changement de mot de passe {#protection}
 * `protect_secret_key(secret, password, options)` renvoie une enveloppe authentifiée de 104 octets.
 * `open_secret_key(envelope, password)` authentifie l'enveloppe avant de restituer les octets secrets.
 * `protected_key_options(envelope)` lit les coûts publics validés sans authentifier l'enveloppe.
 * `change_secret_key_password(envelope, oldPassword, newPassword, optionalCosts)` protège de nouveau la même clé,
 * en conservant les coûts sauf remplacement explicite. Chaque protection utilise un nouveau sel et un nouveau nonce aléatoires.
 * Une ancienne enveloppe reste utilisable avec son ancien mot de passe ; changer le mot de passe ne renouvelle pas les objets du Store
 * et ne révoque pas les autres copies. En cas d'oubli du mot de passe, une autre copie conservée de la clé est nécessaire.
 *
 * Octets de la version 1 : `SYRKEY`, `01 00`, entiers uint32 little-endian pour la mémoire en Kio et les passes, sel de 16 octets,
 * nonce de 24 octets, texte chiffré de 32 octets, tag de 16 octets. XChaCha20-Poly1305 authentifie les 56 premiers octets
 * comme données associées et chiffre seulement le secret. L'en-tête, le sel et le nonce sont publics. Les tailles,
 * versions et coûts hors limites sont refusés avant dérivation ; un mauvais mot de passe ou une altération échoue à l'authentification.
 *
 * Cette API n'enregistre pas les enveloppes et ne modifie pas le format du Store. Conserver enveloppes et mots de passe hors de Git
 * et des fichiers de travail. Les [commandes de sauvegarde](doc:backups) gardent leur contrat de sélection de clé de destination.
 * Voir le [stockage](doc:storage) et les [travaux de sécurité restants](doc:limits#security).
 */
// Argon2id v1.3, one lane, 32 output bytes. Values are checked before allocation:
// 64..256 MiB in whole MiB, 3..10 passes. Passwords are exact borrowed bytes
// (1..4096); salts contain exactly 16 bytes. No Unicode normalization is done.
struct PasswordKdfOptions {
  std::uint32_t memory_kib = 64 * 1024;
  std::uint32_t passes = 3;
  void validate() const;
};

// Move-only ownership of one 32-byte secret. Moving invalidates/wipes the source;
// destruction wipes owned bytes. Views are invalidated by move/destruction;
// temporary owners cannot expose borrowed bytes.
// Caller-owned passwords and copies are the caller's responsibility; no OS vault
// or locked-memory guarantee is implied.
class SecretKey {
public:
  static SecretKey generate();
  static SecretKey from_bytes(std::span<const std::uint8_t>);
  static SecretKey from_password(std::span<const std::uint8_t> password,
      std::span<const std::uint8_t> salt, PasswordKdfOptions = {});
  SecretKey(const SecretKey &) = delete;
  SecretKey &operator=(const SecretKey &) = delete;
  SecretKey(SecretKey &&) noexcept;
  SecretKey &operator=(SecretKey &&) noexcept;
  ~SecretKey();
  std::span<const std::uint8_t, 32> view() const &;
  std::span<const std::uint8_t, 32> view() const && = delete;
private:
  SecretKey() = default;
  Key value_{};
  bool valid_ = false;
};

void wipe_secret(std::span<std::uint8_t>) noexcept;

// Experimental fixed-size v1 envelope (104 bytes): authenticated public KDF
// parameters/salt/nonce and XChaCha20-Poly1305-encrypted 32-byte key. Every seal
// uses a fresh OS-random salt and nonce. Malformed/costly headers fail before KDF.
Bytes protect_secret_key(const SecretKey &, std::span<const std::uint8_t> password,
    PasswordKdfOptions = {});
// Parse public header/costs only. This does not authenticate the envelope.
PasswordKdfOptions protected_key_options(std::span<const std::uint8_t> envelope);
SecretKey open_secret_key(std::span<const std::uint8_t> envelope,
    std::span<const std::uint8_t> password);
// Rewraps the same key with fresh salt/nonce, preserving costs unless overridden.
// Does not rotate Store objects,
// revoke old envelopes, or recover a forgotten password.
Bytes change_secret_key_password(std::span<const std::uint8_t> envelope,
    std::span<const std::uint8_t> old_password,
    std::span<const std::uint8_t> new_password, std::optional<PasswordKdfOptions> = {});
} // namespace syrnck
