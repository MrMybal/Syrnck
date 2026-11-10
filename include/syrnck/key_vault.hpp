#pragma once
#include <syrnck/keys.hpp>
#include <syrnck/signing.hpp>

namespace syrnck {
enum class VaultKeyPurpose { storage = 1, signing = 2, sealed = 3 };

// A public, local reference, not a credential. The OS account and selected
// keychain must also match. References do not make keys portable across hosts.
struct VaultKeyReference {
  VaultKeyPurpose purpose = VaultKeyPurpose::storage;
  std::string id;
  static VaultKeyReference parse(std::string_view);
  std::string encode() const;
};

/**
 * @doc api-system-key-vault
 * @locale en
 * @title System key vault API
 * @section API
 * @symbol syrnck::SystemKeyVault
 * Include `<syrnck/key_vault.hpp>` and link `Syrnck::core`.
 * See [system vaults](doc:key-files#system-vault) for platform and recovery requirements.
 *
 * ## References and providers {#references}
 * `VaultKeyReference::parse` and `encode` validate the exact public format
 * `syrnck-vault:1:storage|signing|sealed:<64 lowercase hex digits>`.
 * `SystemKeyVault::provider()` reports the compiled provider, without opening a vault.
 * macOS file keychains require explicit `disable_macos_interaction()` once before worker threads;
 * this disables all Keychain interaction in the current process. Never re-enable it while vaults are in use.
 * An optional constructor path selects an existing macOS keychain only; other platforms refuse it.
 * Missing providers, sessions, locked vaults and missing entries fail without a file fallback.
 *
 * ## Ownership and recovery {#ownership}
 * `generate`, `save` and `import_protected` create independent entries with random 256-bit IDs.
 * `save` copies a caller-owned 32-byte secret; `import_protected` authenticates a purpose-specific envelope.
 * `open_secret` accepts only storage/sealed references and returns a move-only `SecretKey`.
 * `open_signer` accepts only signing references and returns a move-only `SigningKey`, without seed export.
 * `export_protected` returns an existing purpose-specific password envelope for independent backup.
 * `erase` removes only the selected entry; existing owners and exported backups remain usable.
 * Operations are synchronous. OS/library copies and account policy remain the provider's responsibility.
 * This API does not rotate repository data, revoke distributed keys or supply hardware-bound keys.
 */
/**
 * @doc api-system-key-vault
 * @locale fr
 * @title API des coffres de clés système
 * @section API
 * @symbol syrnck::SystemKeyVault
 * Inclure `<syrnck/key_vault.hpp>` et lier `Syrnck::core`.
 * Voir les [coffres système](doc:key-files#system-vault) pour les plateformes et la récupération.
 *
 * ## Références et fournisseurs {#references}
 * `VaultKeyReference::parse` et `encode` valident le format public exact
 * `syrnck-vault:1:storage|signing|sealed:<64 chiffres hexadécimaux minuscules>`.
 * `SystemKeyVault::provider()` indique le fournisseur compilé sans ouvrir de coffre.
 * Les trousseaux fichiers macOS exigent `disable_macos_interaction()` explicitement avant les threads ;
 * cela désactive toute interaction Trousseau du processus. Ne pas la réactiver pendant l'usage des coffres.
 * Un chemin optionnel au constructeur sélectionne uniquement un trousseau macOS existant ; les autres plateformes le refusent.
 * Un fournisseur ou une session absents, un coffre verrouillé ou une entrée absente échouent sans repli vers un fichier.
 *
 * ## Propriété et récupération {#ownership}
 * `generate`, `save` et `import_protected` créent des entrées indépendantes avec des identifiants aléatoires de 256 bits.
 * `save` copie un secret de 32 octets détenu par l'appelant ; `import_protected` authentifie une enveloppe du bon usage.
 * `open_secret` accepte uniquement les références storage/sealed et rend une `SecretKey` déplaçable.
 * `open_signer` accepte uniquement les références signing et rend une `SigningKey` déplaçable, sans export de graine.
 * `export_protected` rend une enveloppe existante du bon usage, protégée par mot de passe pour une sauvegarde indépendante.
 * `erase` retire uniquement l'entrée choisie ; les propriétaires existants et les sauvegardes exportées restent utilisables.
 * Les opérations sont synchrones. Les copies du système/de sa bibliothèque et les règles du compte relèvent du fournisseur.
 * Cette API ne renouvelle pas les données du dépôt, ne révoque pas les clés distribuées et ne fournit pas de clé matérielle.
 */
class SystemKeyVault {
public:
  explicit SystemKeyVault(std::optional<fs::path> macos_keychain = {});
  static std::string_view provider() noexcept;
  // Explicit process-wide policy on macOS, not an OS-account preference. Call
  // before starting workers; do not re-enable Keychain interaction concurrently.
  // Other platforms do nothing. Vault operations never change this implicitly.
  static void disable_macos_interaction();
  VaultKeyReference generate(VaultKeyPurpose) const;
  VaultKeyReference save(const SecretKey &, VaultKeyPurpose) const;
  VaultKeyReference import_protected(std::span<const std::uint8_t> envelope,
      std::span<const std::uint8_t> password, VaultKeyPurpose) const;
  SecretKey open_secret(const VaultKeyReference &) const;
  SigningKey open_signer(const VaultKeyReference &) const;
  Bytes export_protected(const VaultKeyReference &, std::span<const std::uint8_t> password,
      PasswordKdfOptions = {}) const;
  bool erase(const VaultKeyReference &) const;
private:
  std::optional<fs::path> keychain_;
  SecretKey read(const VaultKeyReference &) const;
};
} // namespace syrnck
