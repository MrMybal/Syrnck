# Syrnck 0.1.0 — notes de version des sources

[English](VERSION.md) · [Français](VERSION.fr.md) · [README](README.fr.md)

## Statut

**Prototype expérimental.** La version `0.1.0` est déclarée par le projet CMake et le CLI. Ces notes décrivent les sources actuelles, pas une release binaire publiée. Aucune release d’application ni aucun installateur n’est actuellement répertorié sur la [page des releases](https://github.com/MrMybal/Syrnck/releases).

Format d’objet v1, formats physiques v1/v2/v3 et ABI C 1 sont des numéros distincts. L’ABI 1 reçoit des points d’entrée supplémentaires : associer bindings et en-têtes à une bibliothèque native correspondante.

## Changements des sources actuelles

- Stockage de blocs C++20 partagé entre assets Git et révisions natives : manifestes complets, blocs réutilisables, plages vérifiées, packs, sauvegardes et rétention explicite.
- Historique natif et opérations récupérables du workspace, serveur de blocs authentifié et transferts ponctuels reprenables.
- Manuel CyDoc local anglais/français, aide contextuelle `syrnck help`, annotations API d’en-têtes publics et documentation intégrée au build, à l’installation et à la CI.
- README anglais/français avec installation depuis les sources, périmètre des licences tierces et présentes notes de version.

Ces fonctions des sources n’annoncent pas de release précompilée. Le [manuel](Documentation/Documentation.html#/fr/overview) détaille les contrats et limites de chaque opération.

## Limites documentées

- HEAD est symbolique. Workspaces natifs multiples/partiels et HEAD détaché restent à terminer.
- Le suivi des renommages à contenu exact existe ; renommages modifiés/copies et intégration au merge/rebase restent incomplets. Les révisions natives n’ont ni auteurs structurés ni signatures ; le rebase préservant les merges reste à terminer.
- L’agent effectue un transfert par invocation. Le watcher observe les changements ; ils ne constituent pas un service complet de synchronisation continue avec versions causales, réconciliation hors ligne, suppressions et propagation des renommages.
- L’adaptateur Syncthing optionnel couvre les données BEP des fichiers ordinaires. Découverte, appairage, vecteurs de versions, conflits et synchronisation complète de daemons/dossiers sont hors de son périmètre.
- XChaCha20-Poly1305 protège les objets au repos. Le serveur de confiance reçoit les objets décodés. Chiffrement de bout en bout, récupération des clés et rotation/révocation distribuées restent à terminer. Les fichiers privés de préparation/checkout peuvent être en clair.
- Manifestes plats et métadonnées de révisions sont limités à 16 Mio. Les budgets de caches et de tâches ne bornent pas toute la RAM. Aucun débit sur projet Unreal réel n’est garanti.
- Archives et autres racines gardent les anciennes versions jusqu’au retrait explicite. Retirer toutes les racines puis collecter les données rend ces versions irrécupérables sans sauvegarde conservée. Politique automatique de rétention et GC distribué restent à terminer.
- La récupération après interruption de processus ne prouve pas la durabilité après coupure ou sur FS réseau. Stabiliser les fichiers pendant les scans et arrêter les écrivains externes pendant le checkout.

Voir les [limites complètes](Documentation/Documentation.html#/fr/limits) et les [formats](Documentation/Documentation.html#/fr/protocol).

## Plateformes

Le build de développement actuel est Windows x64. Des configurations CMake/CI existent pour Windows, Linux et macOS, avec une configuration sanitizer Unix séparée ; elles ne prouvent ni disponibilité d’un binaire publié ni validation de chaque cible. L’exécution Linux/macOS et les autres architectures demandent encore une validation sur cible.

Le binding Go du cœur possède un backend DLL Windows et un backend cgo Unix. Le module Syncthing optionnel a ses exigences séparées. Déclarer un backend ne prouve pas une validation ARM64.

## Bugs et retours

Ces notes ne maintiennent pas de liste de bugs ticket par ticket. Consulter le [suivi des problèmes](https://github.com/MrMybal/Syrnck/issues) pour les signalements et investigations. Pour un problème reproductible, préciser commande, résultat attendu, erreur observée, plateforme et version concernée.

## Références

- [README et installation depuis les sources](README.fr.md#installation).
- [Manuel local](Documentation/Documentation.html#/fr/overview).
- [Dépôt](https://github.com/MrMybal/Syrnck) · [Releases](https://github.com/MrMybal/Syrnck/releases).
- [Licence de l’application](LICENSE) · [Notices tierces](THIRD_PARTY.md).
