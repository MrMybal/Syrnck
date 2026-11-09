---
id: integrations
title: Intégrations des bibliothèques
description: Intégrations des bibliothèques
section: API
order: 700
sourceRevision: 2
---

## C++20 {#cpp}

Installer la construction correspondante avec `cmake --install build --config Release --prefix dist`.
Faire pointer le `CMAKE_PREFIX_PATH` d'une application séparée vers cette installation :

```cmake
cmake_minimum_required(VERSION 3.25)
project(MyApplication LANGUAGES C CXX)
find_package(Syrnck 0.1 CONFIG REQUIRED COMPONENTS Core)
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE Syrnck::core)
```

`cmake -S . -B build -DCMAKE_PREFIX_PATH=/chemin/vers/dist` trouve le paquet
exporté. Il transporte les en-têtes, l'exigence C++20 et les dépendances natives
statiques. L'installation peut être déplacée en bloc : garder ensemble `bin`,
`lib`, `include`, manuel et licences. Ne pas copier une archive seule en devinant
ses dépendances. La construction source expose les mêmes alias publics.

| Cible | Composant | Rôle |
| --- | --- | --- |
| `Syrnck::core` | `Core` | Stockage, révisions/workspaces locaux |
| `Syrnck::git` | `Git` | Filtres Git, catalogues et historique |
| `Syrnck::net` / `Syrnck::git_net` | `Network` | Transferts/serveur HTTP et transferts de catalogues Git |
| `Syrnck::ffi` / `Syrnck::ffi_static` | `C` | ABI C partagée/statique |

Ajouter les composants requis à `find_package` ; demander `Network` à une
installation sans réseau échoue explicitement. Un SDK Unix avec réseau demande
le paquet de développement OpenSSL 3 ; CMake résout aussi les threads système.
Les archives intégrées BLAKE3, Zstandard, Monocypher, utf8proc et curl accompagnent
le SDK. Leurs cibles restent des détails d'implémentation. Une construction sans
réseau (`SYRNCK_BUILD_NETWORK=OFF`, `SYRNCK_BUILD_CLI=OFF`) ne demande ni OpenSSL
ni curl.

Construire un SDK par OS, architecture, ABI compilateur/bibliothèque standard et
runtime/configuration. Faire correspondre Release/Debug et le runtime MSVC ; ce
paquet ne promet pas la compatibilité binaire entre compilateurs.
`Syrnck::ffi_static` propage `SYRNCK_STATIC` et demande un éditeur de liens C++,
même avec un point d'entrée C. L'ABI C partagée sert aux consommateurs utilisant
une autre ABI C++ ; distribuer la bibliothèque de runtime à côté de l'application
ou configurer le chargeur de la plateforme. Les bibliothèques d'import CMake ne
déploient pas automatiquement une DLL Windows.

`ctest --test-dir build -C Release -R '^sdk_distribution$' --output-on-failure`
installe dans un préfixe temporaire, le déplace, puis compile et exécute des
consommateurs indépendants C++, C partagé/statique et réseau facultatif. Ce test
vérifie un aller-retour Store chiffré et l'absence de chemins du dépôt, de build
ou d'installation initiale dans les cibles exportées. Les résultats
multiplateformes demandent l'exécution réelle de ce contrôle.

L’[API Store](doc:api-store) documente verrouillage, vérification et publication. [Progression](doc:operations), [sauvegardes](doc:backups), [diff textuel](doc:text-diff) et [observation](doc:native-watch) détaillent chaque contrat. La référence utilise les annotations explicites des en-têtes publics distribués ; elle ne déduit pas la sémantique des surcharges et n’exporte pas les implémentations.

## C et Go {#c-go}

`syrnck_ffi` exporte l’ABI C ; `syrnck_ffi_static` utilise SYRNCK_STATIC. Faire correspondre binaire et en-tête et ne passer aucun objet C++ à travers l’interface. Respecter les [règles de propriété/erreur](doc:api-c-abi), le [binding Go](doc:go-binding) et les [sessions embarquées](doc:workspace-abi). Windows utilise les appels DLL dynamiques ; Go sur Unix utilise cgo. Sérialiser les handles et récupérer les erreurs sur le thread OS appelant.

L’[adaptateur Syncthing](doc:syncthing) séparé fournit transferts de blocs de fichiers ordinaires, catalogues causaux/tombstones retenus et contrôleur résident de dossier, avec sa référence épinglée. Les appareils sont appairés explicitement ; découverte et métadonnées de dossiers/liens restent à réaliser.

## Plateformes et interopérabilité {#platforms}

Construire séparément pour Windows, Linux, macOS et chaque architecture. Les sources utilisent MSVC ou GCC/Clang avec C++20. Les dépendances natives incluent BLAKE3, Zstandard, Monocypher, utf8proc, JSON et les composants réseau optionnels. Leurs [notices](doc:licenses) accompagnent la distribution.

Pour une intégration minimale, désactiver CLI, réseau, benchmarks et tests ; la génération documentaire peut aussi être désactivée explicitement. Le moteur ne demande ni en-têtes ni runtime Unreal. Un futur plugin est un projet séparé. Lore et Syncthing inspirent certains choix, mais leurs formats ne sont pas ceux de Syrnck. La compatibilité BEP appartient à l’adaptateur explicite, pas aux objets FastCDC/BLAKE3.
