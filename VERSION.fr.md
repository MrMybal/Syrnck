# Syrnck 0.1.0 — notes de version des sources

[English](VERSION.md) · [Français](VERSION.fr.md) · [README](README.fr.md)

## Statut

**Prototype expérimental.** La version `0.1.0` est déclarée par le projet CMake et le CLI. Ces notes décrivent les sources actuelles, pas une release binaire publiée. Aucune release d’application ni aucun installateur n’est actuellement répertorié sur la [page des releases](https://github.com/MrMybal/Syrnck/releases).

Objets anciens v1, révisions facultatives avec auteurs v2, formats physiques v1/v2/v3 et ABI C 1 sont des numéros distincts. L’ABI 1 reçoit des points d’entrée supplémentaires : associer bindings et en-têtes à une bibliothèque native correspondante.

## Changements des sources actuelles

- API de service résident BEP : capture hors ligne indépendante, reconnexion appairée/attente et arrêt joint de TLS/transports ; exécutable autonome et installation OS restent à terminer.

- Réutilisation des manifestes de session pour les captures des agents et C/Go, avec empreintes fiables, inventaire/vérification complète et compteurs sources séparés.

- La reprise explicite à propriétaire conserve les éditions ordinaires pendant le checkout par archives natives et copies exclusives, sans réécrire les cibles entrantes signées.

- Capture, planification de checkout et reprise native contrôlée exposées en C/Go, avec attentes exactes de révision/sélection et liaison Store/contexte.
- Catalogues Syncthing natifs : index causaux, tombstones et historique complet publiés atomiquement ; téléchargements séparés du verrou moteur et reconnexion vérifiée contre le daemon réel.
- SDK C/Go enrichi : création d’objets, préparation mémoire/flux sans références implicites et groupes atomiques de références typées pour retenir les états composés au GC et en sauvegarde.
- Noms natifs macOS NFD des callbacks normalisés en NFC canonique pour index, requêtes de blocs et reconstruction Syncthing.
- Connexions d’appareils BEP Syncthing authentifiées et vérification reproductible contre le daemon amont réel, dont transferts différentiels vers le stockage moteur chiffré.
- Copies de conflit déterministes explicites pour fusions natives et agents bidirectionnels, avec conservation des assets et candidats signés récupérables.
- Stockage de blocs C++20 partagé entre assets Git et révisions natives : manifestes complets, blocs réutilisables, plages vérifiées, packs, sauvegardes et rétention explicite.
- Historique natif et opérations récupérables du workspace, serveur de blocs authentifié et transferts ponctuels reprenables.
- HEAD natif détaché avec index privé, changement récupérable et rétention des historiques détachés.
- Auteurs/committers natifs et signatures Ed25519 facultatives, conservation de l’auteur lors des réécritures et vérification stricte à la lecture/import d’objets.
- Attribution native des lignes à travers les parents de merges et renommages exacts, contenu stocké vérifié et budgets explicites d’entrée/calcul/sortie.
- Manuel CyDoc local anglais/français, aide contextuelle `syrnck help`, annotations API d’en-têtes publics et documentation intégrée au build, à l’installation et à la CI.
- README anglais/français avec installation depuis les sources, périmètre des licences tierces et présentes notes de version.

Ces fonctions des sources n’annoncent pas de release précompilée. Le [manuel](Documentation/Documentation.html#/fr/overview) détaille les contrats et limites de chaque opération.

## Limites documentées

- HEAD nommé/détaché, workspaces liés indépendants, sélection modifiable avec récupération, hard reset/stash partiels et désenregistrement/restauration archivés sont disponibles. Le déplacement lié reste sur le même système de fichiers ; déplacement primaire ou entre systèmes de fichiers reste à terminer.
- Le suivi des renommages à contenu exact existe ; renommages modifiés/copies et intégration au merge/rebase restent incomplets. Les auteurs sont facultatifs ; l’accès serveur v2 peut imposer les clés approuvées, avec enregistrement manuel et redémarrage après changement de politique. Coffre système, révocation distribuée et rebase préservant les merges restent à terminer.
- `agent run` capture en continu changements, suppressions et renommages exacts locaux, conserve des snapshots hors ligne et reprend les publications natives protégées. Le mode facultatif `--bidirectional` applique les descendants distants et fusionne les historiques concurrents compatibles en conservant les deux parents. Conflits non résolus et suppressions/reculs distants reconnus sont bloqués. L’agent reprend ses propres transactions contrôlées hors ligne et conserve l’historique distant observé avant de terminer les publications. Les copies de conflit explicites conservent les contenus concurrents ; l’installation comme service système reste à terminer.
- L’adaptateur Syncthing optionnel couvre les données BEP des fichiers ordinaires et les connexions à appareils explicitement associés. Le catalogue conserve états de dossiers/index et conflits causaux. Son contrôleur continu applique fichiers ordinaires et tombstones aux workspaces natifs, avec reprise par reçus et tests de reconnexion au daemon réel. Métadonnées de dossiers/liens, conflits entre versions BEP concurrentes, orchestration du service autonome, découverte, interface d’appairage, gestion des membres et retrait automatique d’historique restent incomplets.
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
