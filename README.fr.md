# Syrnck

[English](README.md) · [Français](README.fr.md)

**Moteur C++20 de stockage de blocs et de révisions pour les gros assets.**

Syrnck découpe les fichiers en blocs réutilisables adressés par contenu. Chaque version possède un manifeste complet : aucune chaîne de patches binaires n’est nécessaire pour la reconstruire. L’intégration Git garde le code dans Git et représente les assets choisis par des pointeurs. L’historique autonome utilise les mêmes blocs pour commits, index, branches, tags, checkout récupérable, merge, rebase, stash et consultation.

Le moteur propose aussi plages vérifiées, plans de réutilisation, packs indexés, sauvegardes, rétention, serveur authentifié et agent ponctuel reprenable. Bibliothèques C++/C, binding Go et adaptateur BEP Syncthing optionnel pour fichiers ordinaires sont disponibles. Aucun runtime Unreal n’est requis.

**Formats expérimentaux.** Le mode autonome ne couvre pas toutes les fonctions Git. Chiffrement de bout en bout, synchronisation résidente complète et interopérabilité complète des daemons/dossiers Syncthing restent à terminer. Le chiffrement au repos ne cache pas les données au serveur autorisé. Voir les limites actuelles du manuel.

<a id="documentation"></a>

## Manuel local

Le manuel CyDoc complet anglais/français est livré dans `Documentation/` et à côté du binaire installé. Il s’ouvre directement par file:// sans serveur web, CDN ni Internet. Garder l’ensemble du dossier.

```text
syrnck help
syrnck help native tags
syrnck --lang fr help native-checkout
syrnck help packs --section memoire-du-gc --print-url
syrnck --help
```

Ouvrir aussi [Documentation.html](Documentation/Documentation.html) dans un navigateur depuis le checkout ou l’installation. Sur GitHub, consulter les [sources françaises](.CyDocBuild/manual/fr/overview.md) ou [anglaises](.CyDocBuild/manual/en/overview.md) ; GitHub n’exécute pas le lecteur. Recherche, navigation, références de code et liens de section fonctionnent localement. Anglais par défaut ; `--lang` prime sur `SYRNCK_LANG`. Commandes, IDs, JSON et protocoles gardent leur orthographe canonique.

<a id="compiler"></a>

## Construire, tester et installer

Prérequis : CMake 3.25+, compilateurs C++20 et C, Ninja, Python 3.9+, Node.js 22+/npm pour générer le manuel. Windows demande les outils C++ Visual Studio 2022 et un SDK Windows. Les builds réseau Linux/macOS demandent OpenSSL **3.x** de développement. Le script découvre MSVC/CMake/Ninja sous Windows.

```text
python scripts/build.py configure
python scripts/build.py build
python scripts/build.py test
cmake --install build --config Release --prefix dist
```

CLI : `build/Release/syrnck.exe` sous Windows, `build/syrnck` sous Unix. L’installation le place dans `dist/bin` avec `Documentation/`, puis les en-têtes, bibliothèques et licences dans leurs dossiers habituels. La CI distribue ce layout installé. Presets `windows` et `unix` également disponibles dans un environnement compilateur préparé.

Petites dépendances natives et archive CyDoc figée sont incluses. La préparation initiale télécharge Zstd/libcurl vérifiés par hash et les dépendances npm figées, sauf si en cache. Avec Homebrew : `ninja`, `openssl@3` et, si nécessaire, `-DOPENSSL_ROOT_DIR` vers son installation.

Intégration par `add_subdirectory`, [core.hpp](include/syrnck/core.hpp) ou [ABI C](include/syrnck.h). Pour un build minimal, désactiver `SYRNCK_BUILD_CLI`, `SYRNCK_BUILD_NETWORK`, `SYRNCK_BUILD_TESTS` et `SYRNCK_BUILD_BENCHMARKS`. La documentation est alors désactivée par défaut ; `SYRNCK_BUILD_DOCUMENTATION=OFF` la désactive explicitement. L’aide installée ne demande ni Node.js ni Python.

Régénération séparée :

```text
npm --prefix .CyDocBuild ci --no-audit --no-fund --cache .CyDocBuild/.cache/npm
npm --prefix .CyDocBuild run build
npm --prefix .CyDocBuild run check
npm --prefix .CyDocBuild test
```

Maintenance, traductions et vérification navigateur : [.CyDocBuild/README.md](.CyDocBuild/README.md).

## Démarrer un projet

Avec le binaire dans PATH, depuis un dossier projet :

```text
syrnck init
syrnck put Content/Map.uasset
syrnck native commit --message "Premier snapshot du projet"
syrnck refs
syrnck fsck
syrnck gc
```

`put` épingle le manifeste et affiche la réutilisation ; `get MANIFESTE NOUVEAU` reconstruit sans écraser. Le GC prévisualise ; `--delete` collecte physiquement les objets inaccessibles. Conserver branches, archives, pins et sauvegardes vérifiées pour les versions à reconstruire. Commencer par `syrnck --lang fr help usage`, puis `help git-assets` ou `help native-history`.

## Développement et licence

L’exécution locale est Windows x64. La CI prévoit Windows, Linux, macOS et sanitizers Linux, sans constituer une preuve de résultats distants. Mesures et historiques de validation restent hors manuel, sous [.CyDocBuild/internal/records](.CyDocBuild/internal/records).

Avant commit/publication : `python scripts/scan_secrets.py --gitleaks CHEMIN_GITLEAKS`, modes working-tree, staged et history. Conserver les rapports privés sous `.scratch`.

Code original sous [MIT](LICENSE). Les dépendances gardent leurs [licences et notices](THIRD_PARTY.md) ; Syncthing optionnel reste externe sous MPL-2.0. [Lore](https://github.com/EpicGames/lore) et [Syncthing](https://github.com/syncthing/syncthing) inspirent l’architecture ; les formats natifs Syrnck restent distincts.
