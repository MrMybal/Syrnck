# Syrnck

[English](README.md) · [Français](README.fr.md)

L'anglais est la langue par défaut du moteur et des interfaces. Le français est la deuxième langue intégrée : `syrnck --lang fr --help` ou `SYRNCK_LANG=fr`. Les commandes, formats, identifiants et sorties JSON restent stables. Voir la [politique de traduction](docs/LOCALIZATION.md).

Moteur **C++20** de blocs partagés pour le contrôle de versions de projets Unreal Engine et la synchronisation différentielle de fichiers.

Deux modes consomment la même bibliothèque : **Git**, pour conserver son écosystème et ses opérations sur le code ; **autonome**, pour développer progressivement un contrôle de versions indépendant. Un manifeste décrit un fichier complet ; sa reconstruction ne dépend pas d'une chaîne de deltas binaires.

**État : prototype fonctionnel, formats expérimentaux.** Le cœur, les filtres Git, les révisions locales, un serveur et un agent de transfert ponctuel sont exécutables. Le mode autonome n'est pas encore un remplaçant complet de Git. Les performances sur de vrais projets Unreal restent à mesurer.

| Composant | Disponible |
| --- | --- |
| `syrnck_core` | FastCDC 2020 ou blocs fixes, BLAKE3, Zstd, packs indexés, reconstruction vérifiée, staging, checkout récupérable, tags, révisions et GC |
| `syrnck_git` | Filtres simples/résident, catalogues des assets, racines conservées, migration LFS hors ligne et hook pre-push optionnel |
| `syrnck_git_net` | Push/pull groupés, catalogues immuables et hydratation des assets à la demande |
| `syrnck_net` | API HTTP v1 authentifiée, comparaison et transfert des objets absents, publication par compare-and-swap |
| `syrnck` | CLI, serveur et agent push/pull |
| `syrnck_ffi` / `syrnck_ffi_static` | Interface C dynamique/statique : blocs, dépendances, import vérifié et pins |
| `bindings/go` | Binding Go, copie différentielle et reprise entre stores ; DLL Windows / cgo Unix |

Le moteur ne dépend pas d'Unreal. Le chiffrement XChaCha20-Poly1305 protège les objets **au repos** ; le serveur accède au contenu. Le serveur HTTP écoute en loopback et se publie derrière un proxy HTTPS. Le chiffrement de bout en bout et la compatibilité réseau Syncthing restent à développer.

`native diff-text SOURCE CIBLE CHEMIN` compare un fichier vérifié par groupes de lignes UTF-8 bornés ; `--unified` affiche les changements textuels. Les fichiers binaires et limites dépassées sont explicites. Voir [les différences textuelles et leurs limites](docs/TEXT_DIFF.md).

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

`get MANIFESTE NOUVEAU_FICHIER --offset 1048576 --length 65536` exporte une [plage vérifiée](docs/FILE_RANGES.md) en lisant seulement les blocs concernés. `compare ANCIEN_MANIFESTE NOUVEAU_MANIFESTE` produit un [plan de réutilisation](docs/FILE_COMPARISON.md), avec `--summary` pour les seuls compteurs. Ces fonctions existent aussi en C et Go. Une lecture partielle vérifie les blocs sélectionnés ; le hash complet exige une reconstruction complète. Le plan de comparaison ne vérifie pas les données des fichiers.

Pour les gros fichiers, `syrnck put fichier.bin --workers 4 --buffer-mib 32` active la compression et le chiffrement parallèles avec une file bornée. Le mode séquentiel reste le défaut ; les formats et identifiants v1 sont conservés. Voir les [limites et réglages](docs/USAGE.md) et les [mesures](docs/PERFORMANCE.md).

Les réglages du pipeline et les limites des packs sont persistants par dépôt via `config set`. Les options explicites les remplacent pour l'opération, y compris pour `native commit` et `git clean`. Voir les [réglages du dépôt](docs/SETTINGS.md).

Les imports, commandes natives commit/add/staged/reset/status/checkout-plan/checkout/checkout-recover/diff/export, reconstructions, audits `fsck`, filtres Git et transferts de l'agent acceptent `--progress` (anglais/français) ou `--progress-json` sur stderr. Ctrl+C demande une annulation coopérative (code 130). Les exports préparent une arborescence complète avant publication exclusive et préservent les destinations existantes. Une publication déjà engagée peut aboutir. Si la réponse de publication distante est perdue ou ambiguë, le code 3 invite à inspecter la référence. Voir les [contrats des opérations](docs/OPERATIONS.md), [exports transactionnels](docs/EXPORT.md) et [reprises réseau](docs/TRANSFERS.md).

Le cache du moteur évite de répéter le déchiffrement et la décompression d'objets déjà vérifiés, après contrôle de leur enveloppe actuelle. Il est borné à 64 Mio de données et 8 192 empreintes par défaut, configurable avec `--cache-mib` et désactivable avec `--cache-mib 0`. `fsck` et `gc` effectuent leur validation complète sans ce cache.

`syrnck pack` ajoute des packs immuables indexés pour les nouveaux objets ; `pack --compact` regroupe tout le stock. Le GC réécrit seulement les packs partiellement morts et conserve les blocs partagés. Les IDs et le chiffrement restent inchangés. La maintenance active le **stockage physique v2** pour un seul pack, puis **v3** pour plusieurs packs ; les nouveaux stores restent v1 jusqu'à cet appel. Voir [activation, reprise et limites des packs](docs/PACKS.md).

Le lecteur consulte les index de packs par pages vérifiées, avec **4 Mio de cache** par défaut, réglable avec `--index-cache-mib`. Le répertoire de pages et un filtre de clés absentes remplacent la table complète des objets en mémoire. Les formats v2/v3 restent identiques ; l'ouverture vérifie toujours l'index complet. Voir [fonctionnement, garanties et mémoire](docs/INDEXES.md).

Le GC et `fsck` parcourent les inventaires en flux et marquent les objets empaquetés avec **un bit par entrée**. Le GC réécrit les seuls survivants sans liste complète d'IDs. L'empaquetage et la compaction fusionnent les index par pages et préparent les packs sans liste complète d'IDs empaquetés ; les clés isolées restent à trier. Les clés absentes des packs et les métadonnées gardent leurs propres coûts mémoire. Voir les [garanties des audits](docs/AUDIT.md), de la [compaction](docs/COMPACTION.md) et les [budgets du GC](docs/PACKS.md#mémoire-du-gc).

Dans un dépôt Git existant, lancer `syrnck git install`, puis ajouter les règles de [examples/unreal.gitattributes](examples/unreal.gitattributes). Un [hook pre-push optionnel](docs/GIT_ASSETS.md#opt-in-pre-push-hook) peut vérifier et envoyer les assets de tout l'historique à publier. Il faut l'installer explicitement et configurer une URL de blocs distincte. Les [politiques de checkout](docs/GIT_ASSETS.md#on-demand-checkout-and-pointer-only-clones) permettent la reconstruction locale vérifiée, le téléchargement opt-in des blocs manquants ou les clones à pointeurs seuls pour travailler sur le code. Le [filtre résident](docs/GIT_FILTER_PROCESS.md) conserve un Store et son cache borné entre les fichiers d'une même commande Git. Les parcours complets sont dans [docs/USAGE.md](docs/USAGE.md).

Pour un projet LFS existant, `syrnck git lfs-preview HEAD` vérifie les objets du cache local, puis `syrnck git migrate-lfs HEAD --branch converted-assets` crée un commit enfant avec les pointeurs Syrnck et leurs blocs conservés. HEAD, index et fichiers de travail sont préservés. La conversion porte sur un snapshot ; les anciens commits nécessitent toujours leur stockage LFS. Voir la [migration LFS hors ligne et sa récupération](docs/GIT_LFS_MIGRATION.md) en anglais.

`syrnck git retention-scan` inventorie les pointeurs des références, reflogs et index de tous les worktrees enregistrés. `git retention` prévisualise les protections vérifiées ; `git retention --apply` les ajoute en une transaction. Les anciennes protections restent conservées et sont signalées pour inspection. Voir la [couverture et les limites de rétention Git](docs/GIT_RETENTION.md).

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

`native status` compare le contenu du dossier de travail à une branche ou une révision native sans importer les fichiers. `native diff SOURCE CIBLE` compare les métadonnées de deux snapshots, avec distinction du contenu, du mode et du manifeste. Voir les [contrats et limites des comparaisons natives](docs/NATIVE_COMPARISON.md).

`native add CHEMIN...` fige les fichiers et suppressions sélectionnés ; `native staged` les inspecte et `native reset [CHEMIN...]` retire des sélections sans modifier les fichiers de travail. `native commit --staged -m MESSAGE` publie ces octets préparés et consomme le staging dans une transaction atomique. Le GC conserve les données préparées ; un déplacement de la branche impose une réinitialisation explicite. Voir le [staging natif et les commits sélectionnés](docs/NATIVE_STAGING.md). La sélection de patches textuels reste à développer.

`native status --combined` sépare les changements préparés des modifications locales suivantes. `native checkout-plan RÉVISION` indique les actions proposées et les obstacles, y compris les collisions avec des fichiers ignorés, les liens, les variantes de casse et le staging en attente. `--verify` vérifie aussi toute la fermeture stockée de la cible. Voir l'[inspection du workspace natif](docs/NATIVE_WORKSPACE.md).

`native checkout RÉVISION` restaure un workspace suivi propre et déplace la branche sélectionnée, en archivant son ancienne tête et en conservant la cible. `native checkout-status` inspecte une interruption ; `native checkout-recover` reprend l'opération et `--abort` restaure les originaux avant publication. Les déplacements de fichiers sont séquentiels et récupérables grâce au journal. Les fichiers préparés et sauvegardes restent en clair même si les objets sont chiffrés. Il n'y a pas encore d'option force ou de nettoyage automatique des archives de checkout. Voir les [garanties, la rétention et la récupération du checkout](docs/NATIVE_CHECKOUT.md).

`native head` affiche la branche courante ; les anciens stores sélectionnent implicitement `main`. `native switch-plan BRANCHE` prépare le changement et `native switch BRANCHE` restaure puis sélectionne cette branche existante en conservant les deux têtes. Commit, staging, status, log et checkout suivent cette sélection par défaut ; `--branch` la remplace pour l'opération. Le HEAD est publié atomiquement avec le journal. Voir la [sélection de branche native, les valeurs par défaut et la reprise](docs/NATIVE_BRANCHES.md).

`native tag NOM [RÉVISION] [-m MESSAGE]` nomme une version commitée, par défaut HEAD, avec une annotation optionnelle. `native tags` affiche des pages bornées ; `tag-show --verify` vérifie la cible. `tag-delete NOM --expect ID` conserve une archive restaurable après GC, et `tag-restore ARCHIVE_ID` restaure le nom et son message. Les commandes de snapshots acceptent HEAD, les noms de branche/tag non ambigus et les formes explicites `heads/NOM` ou `tags/NOM`. Voir les [tags natifs, sélecteurs et archives conservées](docs/NATIVE_TAGS.md).

`native commit --amend -m MESSAGE` remplace le dernier commit en conservant ses parents et en archivant atomiquement l'ancienne version. `--staged` utilise les octets préparés, `--message-only` conserve le snapshot existant et `--expect ID` exige la tête précédemment inspectée. Les archives conservent tous leurs blocs après GC. Voir les [amendements natifs, la rétention et la reprise après interruption](docs/NATIVE_AMEND.md).

`native ancestry SOURCE CIBLE` indique les deux relations d'ascendance et tous les meilleurs ancêtres communs, y compris les historiques de merges croisés, avec un parcours borné en lecture seule. Voir le [graphe de révisions natif](docs/NATIVE_HISTORY.md).

`native cherry-pick-plan RÉVISION` et `native revert-plan RÉVISION` calculent le rejeu à trois voies par fichier, signalent les conflits et contrôlent le dossier sans écrire d'objet ou de fichier. Les merges exigent le choix explicite du parent principal. Voir les [décisions et limites du rejeu natif](docs/NATIVE_REPLAY.md).

`native cherry-pick RÉVISION -m MESSAGE` et `native revert RÉVISION -m MESSAGE` créent un commit enfant et appliquent ses fichiers via le checkout récupérable. Les changements indépendants et les anciennes versions sont conservés ; les conflits non résolus bloquent l'opération. `--expect ID` exige la tête attendue et une application interrompue peut être reprise ou annulée. Voir [l'application du rejeu et sa récupération](docs/NATIVE_REPLAY.md#apply-a-planned-change).

`native merge-plan RÉVISION` distingue une branche à jour, une avance rapide et un merge divergent, choisit une base commune unique et affiche les conflits de fichiers et du dossier. Plusieurs bases ou des historiques sans lien exigent un choix explicite. Voir la [préparation du merge natif](docs/NATIVE_MERGE.md).

`native merge RÉVISION` applique le résultat via le checkout récupérable : avance rapide exacte ou commit ayant les deux historiques pour parents. `--ff-only`, `--no-ff` et `--expect ID` contrôlent l'intégration ; les anciennes versions restent conservées après GC. Voir [l'application du merge et sa reprise](docs/NATIVE_MERGE.md#apply-and-recover).

`--resolutions FICHIER_JSON` fournit les choix explicites aux merges, cherry-picks, reverts et à leurs plans : version courante, entrante, base, suppression ou manifeste personnalisé importé. Le document exige les révisions et le parent sélectionné attendus ; les conflits restants bloquent toute l'opération. Voir les [résolutions de conflits natives](docs/NATIVE_RESOLUTIONS.md).

`native reset-plan RÉVISION --soft|--mixed` prépare un déplacement de branche ; `native reset RÉVISION --soft|--mixed` l'applique sans écrire les fichiers de travail. Soft conserve les entrées préparées sur la nouvelle base ; mixed archive puis retire l'index. L'ancien historique et les blocs du staging restent conservés après GC. Voir le [reset de branche natif](docs/NATIVE_RESET.md).

`native squash-plan NOMBRE` prépare le regroupement de commits linéaires ; `native squash NOMBRE -m MESSAGE` crée un commit contenant le snapshot exact de l'ancienne tête. Tout l'ancien historique reste archivé après GC. Un merge dans la plage ou un index présent bloque l'opération. Voir le [squash de commits natifs](docs/NATIVE_SQUASH.md).

`native archives` liste l'historique conservé par pages bornées, avec filtre reset, index préparé, squash, amendement, checkout ou détachement. `native archive-show RÉFÉRENCE --verify` vérifie une version conservée avant export ou récupération. Voir les [archives de l'historique natif](docs/NATIVE_ARCHIVES.md).

`native rebase-plan BASE --onto RÉVISION` prépare le rejeu de la plage linéaire explicite `(BASE, HEAD]` sur une autre base. Les nouveaux IDs sont calculés sans stocker de candidats ; messages et dates sont conservés, et le premier conflit ou obstacle du dossier est signalé. Voir le [rebase linéaire natif](docs/NATIVE_REBASE.md).

`native rebase BASE --onto RÉVISION --expect ANCIEN_ID` crée la chaîne de candidats vérifiée puis applique son snapshot final via le checkout récupérable. L'ancien historique reste archivé après GC ; les branches et tags sources sont conservés. Voir [l'application du rebase et sa reprise](docs/NATIVE_REBASE.md#apply-and-recover).

Le rebase accepte aussi `--resolutions FICHIER_JSON` : une série de choix explicites par fichier liée à la tête initiale, aux deux bases et à chaque étape virtuelle. Des choix incomplets ou périmés bloquent toute l'opération. Voir la [résolution des conflits du rebase](docs/NATIVE_REBASE.md#resolve-conflicts-by-step).

`native rebase-index-plan --verify` prépare le rejeu d'un index figé périmé sur la tête courante de sa branche, sans lire les fichiers de travail ni écrire de candidats. Voir le [rebase de l'index natif](docs/NATIVE_STAGE_REBASE.md).

`native rebase-index --expect ID_TÊTE --expect-index ID_INDEX` publie ce résultat atomiquement, en conservant l'ancien index et sa base après GC. Les fichiers de travail et la branche sélectionnée restent intacts.

Les deux commandes acceptent `--resolutions FICHIER_JSON` : des choix par fichier liés à la tête, à l'index figé et à sa base d'origine. Les manifestes/modes personnalisés sont vérifiés avant publication ; des choix incomplets ou périmés ne peuvent pas remplacer l'index.

`native branches --verify` liste des pages bornées de têtes et d'index préparés, en vérifiant les historiques affichés. `native branch NOM RÉVISION` crée désormais une tête entièrement vérifiée sous transaction et refuse les branches existantes, index orphelins et checkouts actifs. Voir les [branches natives](docs/NATIVE_BRANCHES.md#branch-inventory-and-verified-creation).

`native branch-delete-plan` / `branch-delete --expect ID` retirent les branches inactives en archivant atomiquement tout leur historique et les index explicitement autorisés. `native branch-archives` / `branch-archive` consultent ces snapshots conservés après GC. Voir la [suppression archivée de branches](docs/NATIVE_BRANCHES.md#archived-branch-deletion).

`native branch-restore-plan ID_ARCHIVE` / `branch-restore ID_ARCHIVE [--as NOM]` recréent atomiquement les têtes et index archivés dans des destinations libres. Un nouveau nom ne change que le marqueur de branche de l'index ; fichiers préparés, modes et bases restent intacts. Voir la [restauration de branches](docs/NATIVE_BRANCHES.md#restore-an-archived-branch).

La [campagne d'interruption de processus](docs/CRASH_RECOVERY.md) couvre les écritures, les références, l'empaquetage, la compaction, le GC et la reconstruction, en clair et chiffré. La réouverture et la reprise utilisent le moteur de production. Un GC interrompu peut nécessiter de terminer son nettoyage avant qu'un `fsck` complet réussisse. Cette campagne ne simule pas les coupures d'alimentation.

La migration depuis le prototype Rust conserve les identifiants, les manifestes, les frontières FastCDC, les enveloppes chiffrées et les pointeurs Git v1. La lecture et l'écriture croisées des stores physiques v1 ont été vérifiées dans les deux sens. Des fixtures produites par l'ancien moteur sont testées sans installation Rust. L'activation ultérieure des packs requiert le nouveau moteur.

La compilation et les tests locaux sont validés sous Windows x64 avec MSVC. La CI est configurée pour Windows, Linux, macOS et les sanitizers Linux ; ces exécutions distantes n'ont pas été lancées depuis ce dossier. Aucun compilateur ou runtime Rust n'est nécessaire à la version C++.

- [Architecture et garanties](docs/ARCHITECTURE.md)
- [Formats et protocole](docs/PROTOCOL.md)
- [Feuille de route](docs/ROADMAP.md)
- [Mesures reproductibles](docs/PERFORMANCE.md)
- [Grandes tables de références et mesures CLI](docs/REFERENCE_TABLES.md) — anglais
- [Détails de migration C++20](docs/MIGRATION_CPP.md)
- [Binding Go et préparation Syncthing](bindings/go/README.md)

Le projet s'inspire de [Lore](https://github.com/EpicGames/lore) pour le stockage fragmenté et de [Syncthing](https://github.com/syncthing/syncthing) pour les transferts différentiels. Aucun code de ces deux projets n'a été copié dans ce socle.
