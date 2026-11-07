---
id: compatibility
title: Compatibilité et migration C++20
description: Compatibilité et migration C++20
section: Premiers pas
order: 20
sourceRevision: 1
---

## Formats conservés {#formats}

Moteur, CLI, serveur, agent, filtres et ABI C utilisent C++20/CMake. Les dépendances C natives ne demandent aucun compilateur/runtime Rust. La base de compatibilité conserve IDs BLAKE3 typés v1, manifestes/révisions canoniques, frontières FastCDC 2020 (fastcdc 5.0.0, normalisation 1, seed 0), enveloppes SYR1/Zstd/XChaCha20-Poly1305, configuration/références physiques v1, pointeurs Git et HTTP v1. Voir les [formats](doc:protocol).

Les packs récents demandent un lecteur compatible : pack explicite peut activer v2/v3, jamais l’ouverture seule. V3 reste v3 même après réduction des packs. L’accès paginé conserve octets/hashes d’index sans champ disque supplémentaire. Sauvegarder avant d’activer une fonction inconnue d’anciens binaires.

## Différences explicites et distribution {#adaptations}

La normalisation/casefold utf8proc peut refuser des noms équivalents acceptés par un simple lowercase. Les exclusions proposent glob/négations imbriquées avec les limites du scanner. Aucune exception ne traverse C : code 2 signifie exception C++ inconnue ; les appels annulables ajoutent 3. L’ABI 1 gagne des symboles : associer binding, en-têtes et bibliothèque reconstruite compatibles.

HTTPS utilise libcurl/Schannel Windows et OpenSSL 3 Unix ; redirections désactivées. Le service HTTP écoute seulement loopback. Construire pour chaque OS/architecture ; les archives C++ statiques demandent les dépendances au lien final. Définir SYRNCK_STATIC pour l’ABI statique. Voir [intégrations](doc:integrations), [Go](doc:go-binding), [licences](doc:licenses) et les instructions de build du dépôt.
