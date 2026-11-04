# Syrnck

[English](README.md) · [Français](README.fr.md)

L'anglais est la langue par défaut du moteur et des interfaces. Le français est la deuxième langue intégrée : `syrnck --lang fr --help` ou `SYRNCK_LANG=fr`. Les commandes, formats, identifiants et sorties JSON restent stables. Voir la [politique de traduction](docs/LOCALIZATION.md).

Moteur **C++20** de blocs partagés pour le contrôle de versions de projets Unreal Engine et la synchronisation différentielle de fichiers.

Deux modes consomment la même bibliothèque : **Git**, pour conserver son écosystème et ses opérations sur le code ; **autonome**, pour développer progressivement un contrôle de versions indépendant. Un manifeste décrit un fichier complet ; sa reconstruction ne dépend pas d'une chaîne de deltas binaires.

**État : prototype fonctionnel, formats expérimentaux.** Le cœur, les filtres Git, les révisions locales, un serveur et un agent de transfert ponctuel sont exécutables. Le mode autonome n'est pas encore un remplaçant complet de Git. Les performances sur de vrais projets Unreal restent à mesurer.

| Composant | Disponible |
| --- | --- |
| `syrnck_core` | FastCDC 2020 ou blocs fixes, BLAKE3, Zstd, packs indexés, reconstruction vérifiée, révisions et GC |
| `syrnck_git` | Filtres clean/smudge ; code conservé dans Git ; assets représentés par des pointeurs |
| `syrnck_net` | API HTTP v1 authentifiée, comparaison et transfert des objets absents, publication par compare-and-swap |
| `syrnck` | CLI, serveur et agent push/pull |
| `syrnck_ffi` / `syrnck_ffi_static` | Interface C dynamique/statique : blocs, dépendances, import vérifié et pins |
| `bindings/go` | Binding Go, copie différentielle et reprise entre stores ; DLL Windows / cgo Unix |

Le moteur ne dépend pas d'Unreal. Le chiffrement XChaCha20-Poly1305 protège les objets **au repos** ; le serveur accède au contenu. Le serveur HTTP écoute en loopback et se publie derrière un proxy HTTPS. Le chiffrement de bout en bout et la compatibilité réseau Syncthing restent à développer.

## Compiler

Prérequis : CMake 3.25+, compilateur C++20 et compilateur C, Ninja. Sous Windows, installer les outils C++ de Visual Studio 2022 et un SDK Windows. Sous Linux/macOS, les en-têtes et bibliothèques OpenSSL sont aussi nécessaires. Python 3.9+ sert au script de construction et aux tests ; le moteur et le CLI sont des binaires natifs.

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

Le cache du moteur évite de répéter le déchiffrement et la décompression d'objets déjà vérifiés, après contrôle de leur enveloppe actuelle. Il est borné à 64 Mio de données et 8 192 empreintes par défaut, configurable avec `--cache-mib` et désactivable avec `--cache-mib 0`. `fsck` et `gc` effectuent leur validation complète sans ce cache.

`syrnck pack` ajoute des packs immuables indexés pour les nouveaux objets ; `pack --compact` regroupe tout le stock. Le GC réécrit seulement les packs partiellement morts et conserve les blocs partagés. Les IDs et le chiffrement restent inchangés. La maintenance active le **stockage physique v2** pour un seul pack, puis **v3** pour plusieurs packs ; les nouveaux stores restent v1 jusqu'à cet appel. Voir [activation, reprise et limites des packs](docs/PACKS.md).

Le lecteur consulte les index de packs par pages vérifiées, avec **4 Mio de cache** par défaut, réglable avec `--index-cache-mib`. Le répertoire de pages et un filtre de clés absentes remplacent la table complète des objets en mémoire. Les formats v2/v3 restent identiques ; l'ouverture vérifie toujours l'index complet. Voir [fonctionnement, garanties et mémoire](docs/INDEXES.md).

Le GC parcourt les inventaires en flux et marque les objets empaquetés avec **un bit par entrée**, puis réécrit les seuls survivants sans liste complète d'IDs. Les objets vivants isolés et les métadonnées gardent leurs propres coûts mémoire. Voir les [budgets et garanties du GC](docs/PACKS.md#mémoire-du-gc).

Dans un dépôt Git existant, lancer `syrnck git install`, puis ajouter les règles de [examples/unreal.gitattributes](examples/unreal.gitattributes). **Un `git push` ne transfère pas encore les blocs Syrnck** : le transfert reste explicite via l'agent. Les parcours complets sont dans [docs/USAGE.md](docs/USAGE.md).

## Validation et migration

La migration depuis le prototype Rust conserve les identifiants, les manifestes, les frontières FastCDC, les enveloppes chiffrées et les pointeurs Git v1. La lecture et l'écriture croisées des stores physiques v1 ont été vérifiées dans les deux sens. Des fixtures produites par l'ancien moteur sont testées sans installation Rust. L'activation ultérieure des packs requiert le nouveau moteur.

La compilation et les tests locaux sont validés sous Windows x64 avec MSVC. La CI est configurée pour Windows, Linux, macOS et les sanitizers Linux ; ces exécutions distantes n'ont pas été lancées depuis ce dossier. Aucun compilateur ou runtime Rust n'est nécessaire à la version C++.

- [Architecture et garanties](docs/ARCHITECTURE.md)
- [Formats et protocole](docs/PROTOCOL.md)
- [Feuille de route](docs/ROADMAP.md)
- [Mesures reproductibles](docs/PERFORMANCE.md)
- [Détails de migration C++20](docs/MIGRATION_CPP.md)
- [Binding Go et préparation Syncthing](bindings/go/README.md)

Le projet s'inspire de [Lore](https://github.com/EpicGames/lore) pour le stockage fragmenté et de [Syncthing](https://github.com/syncthing/syncthing) pour les transferts différentiels. Aucun code de ces deux projets n'a été copié dans ce socle.
