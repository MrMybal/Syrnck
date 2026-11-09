#pragma once
#include <syrnck/keys.hpp>

namespace syrnck {
/**
 * @doc api-sealed-repositories
 * @locale en
 * @title C++ sealed repositories
 * @section API
 * @symbol syrnck::seal_snapshot
 * ## Stores and ownership {#stores}
 * Include `<syrnck/sealed.hpp>` and link `Syrnck::core`. Keep a move-only `SecretKey`
 * alive through the operation. Plaintext and wire Stores must be distinct, outside
 * each other's directories and outside known plaintext workspaces. Conventional
 * `.syrnck` Stores also protect their enclosing project directory. Serialize Store use.
 * The sealed master key is independent of local at-rest keys.
 *
 * ## Preparation and restoration {#snapshots}
 * `seal_snapshot(source, wire, key, channel, roots, previous, limits, options)` verifies
 * explicit typed roots and retains the prepared opaque carrier at `sealed/prepared/ID`.
 * `previous` binds an exact earlier snapshot; unchanged roots return the same ID.
 * `inspect_sealed_snapshot` authenticates and audits without publishing or importing.
 * `open_sealed_snapshot` verifies/imports original objects, then atomically retains roots
 * and a checkpoint under `sealed/checkpoints/`. It does not replace branches or files.
 * A separate checkpoint permits explicit historical restoration. Failed preparation or
 * import can leave valid unreferenced objects; checkpoints remain unchanged before CAS.
 *
 * ## Remote publication {#network}
 * Include `<syrnck/sealed_net.hpp>` and link `Syrnck::net`. Fetch/push use the existing
 * `Remote`, leases and transfer journals. Push requires server identities/branch policies.
 * Original revision changes must fast-forward unless `allow_rewrite` is explicit; root
 * deletion or non-revision replacement also requires it. Resume uses an exact session.
 * Proven publication is recorded before session completion, even after cancellation.
 * Returned root names/IDs are private: do not send these reports to the wire server.
 *
 * ## Keys, limits and security {#security}
 * `protect_sealed_key`, `open_protected_sealed_key` and `change_sealed_key_password`
 * use a distinct authenticated purpose; password changes preserve the master key/realm.
 * `SealedLimits` bounds objects, work, index caching/new entries and checkpoint history.
 * These are operation bounds, not total RSS or whole-operation differential-work guarantees.
 * See [sealed repositories](doc:sealed-repositories) for metadata leakage, first-contact
 * trust, shared-key permissions, retention and integration limits. The [C
 * ABI](doc:api-c-abi#sealed-repositories) and [Go binding](doc:go-binding#sealed-repositories) expose local
 * preparation/inspection/import; network wrappers remain C++/CLI. Successful publication wins a
 * completion-callback stop request; other observer/IO/output-allocation errors can still follow publication,
 * requiring a ref reload.
 */
/**
 * @doc api-sealed-repositories
 * @locale fr
 * @title Dépôts chiffrés en C++
 * @section API
 * @symbol syrnck::seal_snapshot
 * ## Stores et propriété {#stores}
 * Inclure `<syrnck/sealed.hpp>` et lier `Syrnck::core`. Garder le propriétaire déplaçable
 * `SecretKey` vivant pendant l'opération. Stores en clair et de transport doivent être
 * distincts, hors de leurs répertoires respectifs et des workspaces en clair connus.
 * Les Stores conventionnels `.syrnck` protègent aussi le dossier du projet qui les contient.
 * Sérialiser les accès. La clé de bout en bout est indépendante des clés locales au repos.
 *
 * ## Préparation et restauration {#snapshots}
 * `seal_snapshot(source, wire, key, channel, roots, previous, limits, options)` vérifie
 * les racines typées explicites et retient le support opaque sous `sealed/prepared/ID`.
 * `previous` lie un snapshot antérieur exact ; des racines inchangées rendent le même ID.
 * `inspect_sealed_snapshot` authentifie et audite sans publication ni importation.
 * `open_sealed_snapshot` vérifie/importe les objets originaux, puis retient atomiquement
 * racines et point de contrôle sous `sealed/checkpoints/`. Branches et fichiers restent en place.
 * Un point de contrôle distinct permet la restauration historique explicite. Un échec de
 * préparation/import peut laisser des objets valides sans référence ; les points de contrôle
 * restent inchangés avant le CAS.
 *
 * ## Publication distante {#network}
 * Inclure `<syrnck/sealed_net.hpp>` et lier `Syrnck::net`. Réception/publication réutilisent
 * `Remote`, baux et journaux de transfert. La publication exige identités/règles de branches
 * du serveur. Les révisions originales doivent avancer sans divergence sauf `allow_rewrite`
 * explicite ; suppression ou remplacement de racines non révisionnelles l'exigent aussi.
 * La reprise utilise une session exacte. Une publication prouvée est enregistrée avant de
 * terminer la session, même après annulation. Noms/IDs de racines rendus sont privés :
 * ne pas envoyer ces rapports au serveur de transport.
 *
 * ## Clés, limites et sécurité {#security}
 * `protect_sealed_key`, `open_protected_sealed_key` et `change_sealed_key_password`
 * utilisent une finalité authentifiée distincte ; changer le mot de passe conserve clé/domaine.
 * `SealedLimits` borne objets, travail, cache/nouvelles entrées d'index et historique de contrôle.
 * Ces bornes ne garantissent ni la RAM totale ni un travail global limité aux différences.
 * Voir les [dépôts chiffrés](doc:sealed-repositories) pour métadonnées visibles, confiance initiale,
 * permissions à clé partagée, rétention et limites d'intégration. L'[ABI
 * C](doc:api-c-abi#sealed-repositories) et le [binding Go](doc:go-binding#sealed-repositories) exposent
 * préparation/inspection/import locaux ; les wrappers réseau restent en C++/CLI. Une publication réussie
 * gagne sur l'arrêt demandé dans le callback final ; les autres erreurs d'observateur/IO/allocation imposent
 * encore de relire les références.
 */
// A sealed projection is an ordinary Store containing ciphertext and opaque
// Merkle index nodes. Its HTTP server never receives the client's master key.
// Local Store encryption and the sealed master key are independent.
struct SealedLimits {
  std::size_t max_objects = 4194304;
  std::size_t max_work = 256 * 1024 * 1024;
  std::size_t max_index_cache_bytes = 16 * 1024 * 1024;
  std::size_t max_new_index_bytes = 64 * 1024 * 1024;
  std::size_t max_history = 100000;
  void validate() const;
};

std::string sealed_realm_id(const SecretKey &);
// Password protection has a distinct authenticated purpose from Store keys
// and signing seeds. Rewrapping changes the password, not the repository key.
Bytes protect_sealed_key(const SecretKey &, std::span<const std::uint8_t> password, PasswordKdfOptions = {});
PasswordKdfOptions protected_sealed_key_options(std::span<const std::uint8_t>);
SecretKey open_protected_sealed_key(std::span<const std::uint8_t>, std::span<const std::uint8_t> password);
Bytes change_sealed_key_password(std::span<const std::uint8_t>, std::span<const std::uint8_t> old_password,
                                 std::span<const std::uint8_t> new_password,
                                 std::optional<PasswordKdfOptions> = {});

// Verify source closures before preparing any ciphertext. Previous is an exact
// earlier sealed snapshot in wire, not an implicitly refreshed expectation.
// Retains the prepared carrier through sealed/prepared/ID. Source is read-only.
Json seal_snapshot(const Store &source, const Store &wire, const SecretKey &, std::string_view channel,
                   const Refs &roots, std::optional<std::string> previous = {}, const SealedLimits & = {},
                   OperationOptions = {});

// Authenticate the root and audit its original object/file closure without
// importing or publishing anything. Returned root names/IDs are private data.
Json inspect_sealed_snapshot(const Store &wire, const SecretKey &, std::string_view snapshot,
                             std::string_view channel, const SealedLimits & = {}, OperationOptions = {});

// Import verified original objects, then publish immutable retention roots and
// a local checkpoint atomically. Never replaces native HEADs or working files.
// A checkpoint refuses rollback/forks of its earlier sealed root. A historical
// restore uses a separate checkpoint name rather than weakening that check.
Json open_sealed_snapshot(const Store &wire, const Store &destination, const SecretKey &,
                          std::string_view snapshot, std::string_view channel, std::string_view checkpoint,
                          const SealedLimits & = {}, OperationOptions = {});
} // namespace syrnck
