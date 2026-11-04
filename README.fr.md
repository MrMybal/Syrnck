# Syrnck

[English](README.md) · [Français](README.fr.md)

L'anglais est la langue par défaut du moteur et des interfaces. Le français est la deuxième langue intégrée : `syrnck --lang fr --help` ou `SYRNCK_LANG=fr`. Les commandes, formats, identifiants et sorties JSON restent stables. Voir la [politique de traduction](docs/LOCALIZATION.md).

Moteur **C++20** de blocs partagés pour le contrôle de versions de projets Unreal Engine et la synchronisation différentielle de fichiers.

Deux modes consomment la même bibliothèque : **Git**, pour conserver son écosystème et ses opérations sur le code ; **autonome**, pour développer progressivement un contrôle de versions indépendant. Un manifeste décrit un fichier complet ; sa reconstruction ne dépend pas d'une chaîne de deltas binaires.

**État : prototype fonctionnel, formats expérimentaux.** Le cœur, les filtres Git, les révisions locales, un serveur et un agent de transfert ponctuel sont exécutables. Le mode autonome n'est pas encore un remplaçant complet de Git. Les performances sur de vrais projets Unreal restent à mesurer.

| Composant | Disponible |
| --- | --- |
| `syrnck_core` | FastCDC 2020 ou blocs fixes, BLAKE3, Zstd, packs indexés, reconstruction vérifiée, révisions et GC |
| `syrnck_git` | Filtres simples/résident, catalogues des assets, racines conservées, migration LFS hors ligne et hook pre-push optionnel |
| `syrnck_git_net` | Push/pull groupés, catalogues immuables et hydratation des assets à la demande |
| `syrnck_net` | API HTTP v1 authentifiée, comparaison et transfert des objets absents, publication par compare-and-swap |
| `syrnck` | CLI, serveur et agent push/pull |
| `syrnck_ffi` / `syrnck_ffi_static` | Interface C dynamique/statique : blocs, dépendances, import vérifié et pins |
| `bindings/go` | Binding Go, copie différentielle et reprise entre stores ; DLL Windows / cgo Unix |

Le moteur ne dépend pas d'Unreal. Le chiffrement XChaCha20-Poly1305 protège les objets **au repos** ; le serveur accède au contenu. Le serveur HTTP écoute en loopback et se publie derrière un proxy HTTPS. Le chiffrement de bout en bout et la compatibilité réseau Syncthing restent à développer.

## Compiler

Prérequis : CMake 3.25+, compilateur C++20 et compilateur C, Ninja. Sous Windows, installer les outils C++ de Visual Studio 2022 et un SDK Windows. Sous Linux/macOS, les en-têtes et bibliothèques OpenSSL **3.x** sont aussi nécessaires. Python 3.9+ sert au script de construction et aux tests ; le moteur et le CLI sont des binaires natifs.

```text
python scripts/build.py configure
python scripts/build.py build
python scripts/build.py test
```

Le script découvre MSVC/CMake/Ninja sous Windows. Depuis un environnement de compilation déjà préparé, on peut aussi employer directement les presets :

```text
cmake --preset windows
cmake --build --preset windows
ctest --preset windows
```

Employer le preset `unix` sur Linux/macOS. Les petites dépendances sont incluses ; CMake télécharge Zstd et libcurl depuis des archives versionnées vérifiées par SHA-256. Voir [THIRD_PARTY.md](THIRD_PARTY.md).

Le CLI se trouve dans `build/Release/syrnck.exe` sous Windows et `build/syrnck` avec le preset Unix. La bibliothèque C++ s'utilise via [include/syrnck/core.hpp](include/syrnck/core.hpp), l'interface C via [include/syrnck.h](include/syrnck.h). Les cibles CMake sont disponibles en ajoutant le projet avec `add_subdirectory`.

## Utiliser

Avec le dossier du binaire dans le `PATH`, depuis un projet :

```text
syrnck init
syrnck put Content/Map.uasset
syrnck native commit --message "Premier état du projet"
syrnck refs
syrnck fsck
syrnck gc
```

`put` affiche le manifeste et les octets réutilisés. `get <manifeste> <nouveau-fichier>` reconstruit un fichier. Le GC est simulé par défaut ; `gc --delete` supprime les objets inaccessibles. Révisions, branches, archives et pins protègent leurs blocs.

Pour les gros fichiers, `syrnck put fichier.bin --workers 4 --buffer-mib 32` active la compression et le chiffrement parallèles avec une file bornée. Le mode séquentiel reste le défaut ; les formats et identifiants v1 sont conservés. Voir les [limites et réglages](docs/USAGE.md) et les [mesures](docs/PERFORMANCE.md).

Les réglages du pipeline et les limites des packs sont persistants par dépôt via `config set`. Les options explicites les remplacent pour l'opération, y compris pour `native commit` et `git clean`. Voir les [réglages du dépôt](docs/SETTINGS.md).

Les imports, commits/exports natifs, reconstructions, audits `fsck`, filtres Git et transferts de l'agent acceptent `--progress` (anglais/français) ou `--progress-json` sur stderr. Ctrl+C demande une annulation coopérative (code 130). Les exports préparent une arborescence complète avant publication exclusive et préservent les destinations existantes. Une publication déjà engagée peut aboutir. Si la réponse de publication distante est perdue ou ambiguë, le code 3 invite à inspecter la référence. Voir les [contrats des opérations](docs/OPERATIONS.md), [exports transactionnels](docs/EXPORT.md) et [reprises réseau](docs/TRANSFERS.md).

Le cache du moteur évite de répéter le déchiffrement et la décompression d'objets déjà vérifiés, après contrôle de leur enveloppe actuelle. Il est borné à 64 Mio de données et 8 192 empreintes par défaut, configurable avec `--cache-mib` et désactivable avec `--cache-mib 0`. `fsck` et `gc` effectuent leur validation complète sans ce cache.

`syrnck pack` ajoute des packs immuables indexés pour les nouveaux objets ; `pack --compact` regroupe tout le stock. Le GC réécrit seulement les packs partiellement morts et conserve les blocs partagés. Les IDs et le chiffrement restent inchangés. La maintenance active le **stockage physique v2** pour un seul pack, puis **v3** pour plusieurs packs ; les nouveaux stores restent v1 jusqu'à cet appel. Voir [activation, reprise et limites des packs](docs/PACKS.md).

Le lecteur consulte les index de packs par pages vérifiées, avec **4 Mio de cache** par défaut, réglable avec `--index-cache-mib`. Le répertoire de pages et un filtre de clés absentes remplacent la table complète des objets en mémoire. Les formats v2/v3 restent identiques ; l'ouverture vérifie toujours l'index complet. Voir [fonctionnement, garanties et mémoire](docs/INDEXES.md).

Le GC et `fsck` parcourent les inventaires en flux et marquent les objets empaquetés avec **un bit par entrée**. Le GC réécrit les seuls survivants sans liste complète d'IDs. L'empaquetage et la compaction fusionnent les index par pages et préparent les packs sans liste complète d'IDs empaquetés ; les clés isolées restent à trier. Les clés absentes des packs et les métadonnées gardent leurs propres coûts mémoire. Voir les [garanties des audits](docs/AUDIT.md), de la [compaction](docs/COMPACTION.md) et les [budgets du GC](docs/PACKS.md#mémoire-du-gc).

Dans un dépôt Git existant, lancer `syrnck git install`, puis ajouter les règles de [examples/unreal.gitattributes](examples/unreal.gitattributes). Un [hook pre-push optionnel](docs/GIT_ASSETS.md#opt-in-pre-push-hook) peut vérifier et envoyer les assets de tout l'historique à publier. Il faut l'installer explicitement et configurer une URL de blocs distincte. Les [politiques de checkout](docs/GIT_ASSETS.md#on-demand-checkout-and-pointer-only-clones) permettent la reconstruction locale vérifiée, le téléchargement opt-in des blocs manquants ou les clones à pointeurs seuls pour travailler sur le code. Le [filtre résident](docs/GIT_FILTER_PROCESS.md) conserve un Store et son cache borné entre les fichiers d'une même commande Git. Les parcours complets sont dans [docs/USAGE.md](docs/USAGE.md).

Pour un projet LFS existant, `syrnck git lfs-preview HEAD` vérifie les objets du cache local, puis `syrnck git migrate-lfs HEAD --branch converted-assets` crée un commit enfant avec les pointeurs Syrnck et leurs blocs conservés. HEAD, index et fichiers de travail sont préservés. La conversion porte sur un snapshot ; les anciens commits nécessitent toujours leur stockage LFS. Voir la [migration LFS hors ligne et sa récupération](docs/GIT_LFS_MIGRATION.md) en anglais.

L'agent accepte `--requests 4 --transfer-buffer-mib 64` pour chevaucher les requêtes d'objets, avec accès au Store coordonnés. Le défaut reste une requête ; le budget couvre les payloads et les réponses réservées, pas toute la mémoire du processus. Voir [limites et reprises des transferts parallèles](docs/PARALLEL_TRANSFERS.md). Le GC et l'empaquetage deviennent interruptibles pendant la préparation, puis terminent leur nettoyage après publication ; voir les [frontières de maintenance](docs/MAINTENANCE.md).

Les requêtes en erreur temporaire sont relancées jusqu'à trois fois par défaut (`--retries 0..8`). Les options `--upload-kib-s` et `--download-kib-s` fixent des cibles de débit moyen partagées entre les requêtes d'objets ; zéro signifie illimité. Ces attentes sont annulables. La publication finale d'une référence n'est jamais relancée automatiquement. Voir la [politique de reprise et de débit](docs/TRANSFER_POLICY.md).

Les transferts enregistrent un journal local persistant. Les commandes `agent sessions`, `agent resume URL SESSION_ID` et `agent forget SESSION_ID` permettent de le consulter, de reprendre ou de l'abandonner explicitement. Les sessions inachevées protègent leur source complète ou les objets déjà téléchargés pendant le GC local, et conservent la référence attendue avant l'interruption. `agent prune` prévisualise les reçus terminés ; `agent prune --delete` les supprime en conservant les sessions inachevées, les références et les données. Voir les [sessions et la récupération des publications](docs/TRANSFER_SESSIONS.md) et la [rétention sélective](docs/SELECTIVE_RETENTION.md).

Les serveurs compatibles protègent aussi les envois avec des baux persistants, renouvelables et expirables, et refusent les anciens jetons de publication. `--require-lease` exige cette protection ; `leases list`, `leases release ID` et `leases prune` administrent les baux hors ligne. Les reçus permettent de retrouver une publication sans écraser une référence modifiée ensuite. Voir les [baux d'envoi serveur](docs/SERVER_UPLOAD_LEASES.md).

Les nouveaux envois négocient des [inventaires paginés](docs/UPLOAD_INVENTORIES.md), enregistrés avant les demandes de présence. Le GC serveur conserve les blocs déclarés même avant l'arrivée du manifeste, et peut supprimer les données sans lien avec l'envoi. Les anciens baux gardent leur protection globale ; `leases prune` nettoie aussi les générations d'inventaire obsolètes.

Les téléchargements acquièrent aussi des [baux de lecture](docs/SERVER_READ_LEASES.md) sur la racine source exacte avant de demander les blocs manquants. Ils survivent au redémarrage du serveur et à la suppression de la référence jusqu'à expiration ou libération. `--require-read-lease` exige cette protection ; `serve --read-lease-seconds 3600` règle leur durée et `leases ... --read` permet leur administration hors ligne.

Les transferts négocient la [compression Zstandard](docs/NETWORK_COMPRESSION.md)
par défaut. `--compression auto|off|zstd` la règle côté client et
`serve --no-compression` la désactive côté serveur. Le décodage est borné et les
objets incompressibles peuvent rester bruts ; les compteurs distinguent octets
logiques et trafic des corps d'objets.

## Validation et migration

La [campagne d'interruption de processus](docs/CRASH_RECOVERY.md) couvre les écritures, les références, l'empaquetage, la compaction, le GC et la reconstruction, en clair et chiffré. La réouverture et la reprise utilisent le moteur de production. Un GC interrompu peut nécessiter de terminer son nettoyage avant qu'un `fsck` complet réussisse. Cette campagne ne simule pas les coupures d'alimentation.

La migration depuis le prototype Rust conserve les identifiants, les manifestes, les frontières FastCDC, les enveloppes chiffrées et les pointeurs Git v1. La lecture et l'écriture croisées des stores physiques v1 ont été vérifiées dans les deux sens. Des fixtures produites par l'ancien moteur sont testées sans installation Rust. L'activation ultérieure des packs requiert le nouveau moteur.

La compilation et les tests locaux sont validés sous Windows x64 avec MSVC. La CI est configurée pour Windows, Linux, macOS et les sanitizers Linux ; ces exécutions distantes n'ont pas été lancées depuis ce dossier. Aucun compilateur ou runtime Rust n'est nécessaire à la version C++.

- [Architecture et garanties](docs/ARCHITECTURE.md)
- [Formats et protocole](docs/PROTOCOL.md)
- [Feuille de route](docs/ROADMAP.md)
- [Mesures reproductibles](docs/PERFORMANCE.md)
- [Détails de migration C++20](docs/MIGRATION_CPP.md)
- [Binding Go et préparation Syncthing](bindings/go/README.md)

Le projet s'inspire de [Lore](https://github.com/EpicGames/lore) pour le stockage fragmenté et de [Syncthing](https://github.com/syncthing/syncthing) pour les transferts différentiels. Aucun code de ces deux projets n'a été copié dans ce socle.
