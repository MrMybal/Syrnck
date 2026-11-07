---
id: native-squash
title: Regroupement de commits
description: Guide : Regroupement de commits.
section: Versionnage
order: 360
parent: native-history
sourceRevision: 1
---

# Regrouper des commits natifs {#native-commit-squash}

Squash remplace les 2 à 100000 derniers commits linéaires par un seul, avec exactement les entrées de fichiers de l’ancienne tête. Il ne change ni fichiers préparés ni workspace et conserve tout l’historique d’origine dans une archive.

La plage inclut la tête et suit ses parents uniques. Le nouveau parent est celui du commit le plus ancien inclus, ou aucun si la racine est atteinte. Manifestes, bits exécutables et représentations viennent de l’ancienne tête, sans reconstruction ni réingestion pour créer le candidat. Message obligatoire, date UTC courante et limite de révision de 16 Mio s’appliquent.

Une fusion dans la plage bloque avec `merge_in_range` : aucun parent n’est choisi silencieusement. Une fusion immédiatement avant la plage reste le nouveau parent avec toute son ascendance. Une plage trop longue donne `count_exceeds_history`. Tout index, même propre, une récupération checkout en cours ou une archive contradictoire bloque. Terminer ou retirer explicitement l’index auparavant.

Le plan en lecture seule expose `head`, `count`, `revisions` du plus récent au plus ancien, `parents` proposés, nombre de fichiers, référence d’archive, `ready` et blocages. Une plage incomplète a des parents null. Code 0 si prêt, 1 avec JSON si bloqué ; comptes/métadonnées invalides produisent un diagnostic. La sortie est bornée à 16 Mio ; la mémoire dépend des IDs visités et des révisions plates décodées.

`--verify` contrôle toute la fermeture de l’ancienne tête et les hashes de fichiers. L’application le fait toujours avant candidat, puis vérifie remplacement et archive. Un aperçu de métadonnées ne prouve pas la présence des blocs. Un bloc d’ancêtre manquant bloque même si la nouvelle tête n’en a plus besoin.

```text
syrnck native squash-plan 3 --verify
syrnck native squash 3 -m "Implement asset streaming" --expect OLD_TIP_ID
```


## Publication et historique conservé {#publication-and-retained-history}

Une seule transaction déplace la branche et crée `archive/squash/OLD_HEAD_ID` pour toute la chaîne d’origine. L’absence attendue de l’index est contrôlée ensemble. Tags, autres branches et sélection HEAD ne changent pas. Les modifications de travail ne sont ni lues ni incluses.

`--branch` choisit la branche ; `--expect` est l’ID exact de l’ancienne tête, capturé sous verrou si omis au CLI et toujours requis en C++. Une coupure laisse toutes les anciennes ou nouvelles références. Une annulation/erreur de callback avant publication conserve l’ancien état mais peut laisser un candidat immuable non référencé. Aucun journal de fichiers n’existe. Inspecter tête et références avant nouvel essai avec l’ancien ID ; aucun callback ni contrôle d’arrêt après publication.

Les [archives](doc:native-archives) permettent de retrouver têtes et versions préparées. Elles n’expirent pas automatiquement et gardent toutes les versions intermédiaires après GC. Exporter une révision ou créer une branche pour récupérer les fichiers. Le [retrait avec sauvegarde](doc:archive-retention) est explicite ; squash raccourcit l’historique visible sans récupérer automatiquement l’espace archivé. Retirer la dernière racine autorise la collecte des données exclusives.

Le format v1 reste identique, sans auteur ni signature. C++ et CLI appliquent progression bilingue, annulation coopérative et callbacks non réentrants. L’historique natif n’est pas encore exposé dans les bindings C/Go.

```cpp
auto preview = store.plan_squash("main", 3, true, operation);
auto applied = store.squash_commits("main", expected_id, 3,
                                    "Implement asset streaming", operation);
```
