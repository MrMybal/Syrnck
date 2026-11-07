---
id: audit
title: Vérification d’intégrité
description: Guide : Vérification d’intégrité.
section: Stockage
order: 40
parent: storage
sourceRevision: 1
---

# Audits d’intégrité en flux {#streaming-integrity-audits}

`Store::fsck()` et `syrnck fsck` vérifient tous les objets logiques actifs, y compris les révisions, manifestes et blocs inaccessibles. L’audit est en lecture seule ; le GC contrôle la rétention et les suppressions. Après un GC interrompu, des métadonnées mortes isolées peuvent temporairement référencer des blocs morts déjà retirés : voir la [reprise du nettoyage](doc:crash-recovery#interrupted-garbage-collection).

L’audit recharge la configuration et tous les index actifs, contourne les caches décodés et de validation, vérifie les références puis énumère révisions, manifestes et blocs sans inventaire complet d’IDs. Les bornes de pages sélectionnent les types ; les enregistrements mixtes restent filtrés à leur position d’origine. Les métadonnées précèdent les blocs autonomes : chaque hash complet est calculé en lisant les blocs dans l’ordre. Un bloc partagé ou répété peut être relu pour plusieurs fichiers ; aucune garantie d’une lecture unique par objet n’est donnée.

Chaque lecture contrôle enveloppe, authentification chiffrée, ID et format. Dépendances absentes ou hash complet incorrect font échouer l’audit, même dans les métadonnées inaccessibles. Un doublon isolé/empaqueté compte une fois mais ses deux copies sont contrôlées. Un pack inactif est hors inventaire et ne retient rien.

## Mémoire et durée de vie {#memory-and-lifetime}

Les objets empaquetés utilisent un bit par entrée, arrondi séparément par pack : 131 072 octets pour 1 048 576 entrées. Répertoire, filtre et cache de pages ont leurs propres coûts : voir les [index](doc:indexes). Les objets absents des packs nécessitent un ensemble compact d’IDs typés. Références, pile de dépendances, JSON, allocations et objets décodés s’ajoutent ; un manifeste ou une révision reste matérialisé. La mémoire totale n’est pas indépendante de la taille du projet.

Chaque page possède son tampon, conservé lors des lectures imbriquées même sans cache. Les sessions partagent un nombre borné de handles, fermés au retour extérieur ou sur exception. La politique ordinaire du cache décodé est rétablie après succès ou erreur. Le verrou exclusif couvre l’opération. `objects()` et `closure()` retournent encore des collections complètes ; la [compaction](doc:compaction) utilise des plans en flux et trie les clés isolées. Aucun format de stockage, réseau ou ABI ne change.

## Interpréter les ressources {#verification-and-measurement}

Voir les [budgets et limites de performance](doc:performance) pour les compteurs de mémoire et caches. Un corpus synthétique ne prouve ni débit ni capacité sur un projet Unreal. L’audit contrôle les octets réellement stockés indépendamment des statistiques de cache.

## Progression et annulation {#progress-and-cancellation}

`fsck` accepte `OperationOptions`, `--progress` et `--progress-json`. Les compteurs suivent les lectures décodées sans matérialiser l’inventaire. L’interruption ferme les sessions et rétablit le cache ; objets et rétention restent inchangés. Voir le [contrat des opérations](doc:operations).
