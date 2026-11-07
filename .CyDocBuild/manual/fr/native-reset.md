---
id: native-reset
title: Reset soft et mixed
description: Guide : Reset soft et mixed.
section: Versionnage
order: 340
parent: native-history
sourceRevision: 1
---

# Reset soft et mixed d’une branche {#native-branch-reset}

Le reset déplace une branche existante vers une révision sans toucher aux fichiers de travail. Ancienne tête et éventuel index sont archivés dans la même transaction. `native reset [PATH...]` garde son sens de retrait de sélection ; déplacer une branche exige un mode explicite.

`--soft` conserve les entrées préparées et rebase leur base sur la cible. Sans index, il fige les entrées complètes de l’ancienne tête sur la nouvelle base, permettant ensuite un commit staged enfant de la cible. Les octets préparés restent distincts des modifications ultérieures du workspace. Un index périmé bloque soft ; `--mixed` l’archive puis le vide. Les deux refusent un index dont l’identité ou les métadonnées sont invalides.

La cible peut être toute révision native valide, sans contrainte d’ascendance. Le CLI accepte les sélecteurs usuels et `--branch` sans changer HEAD. L’API utilise des IDs exacts et une branche existante. `--expect` protège l’ancienne tête ; sans lui, le CLI la capture sous verrou. L’index est capturé à l’invocation et contrôlé à la publication, pas figé par un ancien aperçu.

Les plans rendent du JSON : code 0 si prêt, code 1 avec blocages sinon. Par défaut seules les métadonnées sont lues ; `--verify` vérifie fermetures complètes tête/cible/index et hashes de fichiers. Le plan soft comprend la comparaison proposée contre la cible. Rapports et révisions gardent le plafond de 16 Mio ; la mémoire croît avec les snapshots plats et la comparaison bornée.

Le workspace n’est pas analysé : modifications et non-suivis demeurent disponibles. Récupération checkout/switch, archives contradictoires ou index soft périmé bloquent. Vers la même tête, le reset ne fait rien, sauf mixed qui archive et retire tout index existant.

```text
syrnck native reset-plan REVISION --soft --verify
syrnck native reset REVISION --soft --expect OLD_TIP_ID
syrnck native reset REVISION --mixed --expect OLD_TIP_ID
```


## Rétention et interruption {#retention-and-interruption}

L’ancienne tête reste sous `archive/reset/OLD_ID`, l’ancien index sous `archive/reset-index/INDEX_ID`, avec son historique de base. Parents, manifestes et blocs survivent au GC en clair/chiffré et isolés/empaquetés. Tags et autres branches ne bougent pas. Toutes les fermetures anciennes et nouvelles sont vérifiées avant publication ; un ancien bloc absent empêche aussi mixed de retirer sa seule protection.

Branche, index et deux archives sont publiés par remplacement atomique de `refs.json`. Annulation ou erreur d’observateur conservent les références anciennes ; la préparation soft peut laisser un index immuable non référencé. Une coupure laisse l’ensemble ancien ou nouveau. Inspecter références, tête et index avant nouvel essai avec l’ancienne tête attendue. Aucun journal de fichiers n’est nécessaire puisque le workspace ne change pas. Aucun callback ni contrôle d’arrêt ne suit la publication.

Retrouver les versions dans les [archives](doc:native-archives), exporter leur ID ou créer une branche de récupération. Sans expiration automatique, elles conservent les versions intermédiaires. Retirer la dernière racine puis collecter supprime les données exclusives. Les fichiers jamais ingérés ne sont pas sauvegardés par reset. Pour remplacer le workspace, utiliser le [reset hard récupérable](doc:native-hard-reset), une opération distincte.

Le format v1 ne change pas. Les API C++ et CLI proposent progression/diagnostics bilingues, JSON anglais et callbacks non réentrants ; l’exposition de l’historique natif en C/Go reste à développer.

```text
syrnck native export OLD_ID recovered-version
syrnck native branch recovered OLD_ID
syrnck native export INDEX_ID recovered-prepared-files
```


```cpp
auto plan = store.plan_reset_branch("main", target_id, syrnck::NativeResetMode::soft,
                                    true, operation);
auto result = store.reset_branch("main", expected_id, target_id,
                                 syrnck::NativeResetMode::soft, operation);
```
