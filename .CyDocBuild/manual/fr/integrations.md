---
id: integrations
title: Intégrations des bibliothèques
description: Intégrations des bibliothèques
section: API
order: 700
sourceRevision: 1
---

## C++20 {#cpp}

Utiliser les en-têtes distribués et les cibles CMake. `syrnck_core` fournit stockage et opérations natives locales ; `syrnck_git` ajoute Git ; `syrnck_net` ajoute HTTP ; `syrnck_git_net` relie les catalogues Git aux transferts. L’alias `Syrnck::core` est disponible dans la construction source. Les archives C++ statiques demandent aussi l’édition de liens des dépendances natives.

L’[API Store](doc:api-store) documente verrouillage, vérification et publication. [Progression](doc:operations), [sauvegardes](doc:backups), [diff textuel](doc:text-diff) et [observation](doc:native-watch) détaillent chaque contrat. La référence utilise les annotations explicites des en-têtes publics distribués ; elle ne déduit pas la sémantique des surcharges et n’exporte pas les implémentations.

## C et Go {#c-go}

`syrnck_ffi` exporte l’ABI C ; `syrnck_ffi_static` utilise SYRNCK_STATIC. Faire correspondre binaire et en-tête et ne passer aucun objet C++ à travers l’interface. Respecter les [règles de propriété/erreur](doc:api-c-abi), le [binding Go](doc:go-binding) et les [sessions embarquées](doc:workspace-abi). Windows utilise les appels DLL dynamiques ; Go sur Unix utilise cgo. Sérialiser les handles et récupérer les erreurs sur le thread OS appelant.

L’[adaptateur Syncthing](doc:syncthing) séparé couvre les données de blocs de fichiers ordinaires avec sa dépendance de référence épinglée. Il ne gère ni association d’appareils, ni versions causales, ni suppressions, ni daemon résident.

## Plateformes et interopérabilité {#platforms}

Construire séparément pour Windows, Linux, macOS et chaque architecture. Les sources utilisent MSVC ou GCC/Clang avec C++20. Les dépendances natives incluent BLAKE3, Zstandard, Monocypher, utf8proc, JSON et les composants réseau optionnels. Leurs [notices](doc:licenses) accompagnent la distribution.

Pour une intégration minimale, désactiver CLI, réseau, benchmarks et tests ; la génération documentaire peut aussi être désactivée explicitement. Le moteur ne demande ni en-têtes ni runtime Unreal. Un futur plugin est un projet séparé. Lore et Syncthing inspirent certains choix, mais leurs formats ne sont pas ceux de Syrnck. La compatibilité BEP appartient à l’adaptateur explicite, pas aux objets FastCDC/BLAKE3.
