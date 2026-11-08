---
id: native-branches
title: Branches et changement de branche
description: Guide : Branches et changement de branche.
section: Versionnage
order: 220
parent: native-history
sourceRevision: 2
---

# Branches natives et changement récupérable {#native-branches-and-recoverable-switching}

Le CLI sélectionne normalement une branche nommée et prend aussi en charge [HEAD détaché](doc:native-detached). Sans descripteur HEAD, un ancien Store sélectionne implicitement `main`, même non née. Lire HEAD ne le crée pas. Un switch réussi enregistre la sélection liée au répertoire canonique.

`native branch NAME REVISION` crée sans sélectionner ni modifier les fichiers. `native switch NAME` restaure la tête exacte et sélectionne la branche sans déplacer aucune tête. `native checkout REVISION` restaure une révision et déplace la cible sélectionnée en archivant son ancienne tête. Les deux utilisent le [checkout récupérable](doc:native-checkout). [`native switch --detach REVISION`](doc:native-detached) sélectionne une révision indépendamment des branches. Switch ne crée jamais de branche nommée.

```text
syrnck native head
syrnck native branch feature REVISION
syrnck native switch-plan feature --verify
syrnck native switch feature --progress
syrnck native status --combined
syrnck native add Source Content/Maps
syrnck native commit --staged -m "Selected feature changes"
syrnck native log
syrnck native switch main
```


## Inventaire et création vérifiée {#branch-inventory-and-verified-creation}

`native branches` parcourt les références `heads/` triées par octets. Le JSON contient `head`, `branches`, `has_more`, le curseur exclusif `next_after` et `objects_verified`. Une branche sélectionnée non née apparaît dans `head` sans ligne fictive. Une page vide a un curseur nul. `--limit` vaut 1..1000, défaut 100 ; le curseur peut avoir disparu. Les appels successifs ne forment pas un snapshot partagé.

Chaque ligne comprend nom, révision, sélection courante, nombres de fichiers/parents, timestamp et aperçu UTF-8 du message limité à 256 octets avec `message_bytes`/`message_truncated`. `index`, `index_base`, `stale_index` exposent l’index figé, même périmé, sans lire les fichiers. Les index orphelins restent accessibles par `refs` ou `native staged --branch NAME`.

Sans `--verify`, les payloads ne sont pas audités. Avec cette option, les fermetures complètes des têtes et index affichés, historiques de base compris, sont contrôlées ; la tête sélectionnée hors page n’est pas une racine supplémentaire. Rapports limités à 16 Mio. La table des références est chargée une fois, entièrement, sans copie par branche.

La création C++ exige une révision exacte existante ; le CLI accepte aussi les sélecteurs. Elle vérifie toutes les données retenues et publie seulement si tête et slot d’index sont absents. Une tête existante, un index orphelin ou un checkout/switch actif bloque ; aucun remplacement forcé. Les fichiers sales restent inchangés. Si HEAD nommait déjà cette branche non née, il résout ensuite la nouvelle tête.

La création CLI reste silencieuse sur stdout ; C++ retourne nom et révision. L’inventaire reste JSON anglais quelle que soit la langue. Progression et annulation ordinaires s’appliquent. Verrou exclusif et attentes d’absence protègent la publication ; annulation, données absentes ou observer en échec avant publication préservent références et workspace. Aucun callback après ce point.

```text
syrnck native branches --limit 100 --verify
syrnck native branches --after feature/topic --limit 100
syrnck native branch feature/topic REVISION --progress
```


## Suppression archivée {#archived-branch-deletion}

La suppression d’une branche inactive retire `heads/NAME` et éventuellement `staging/NAME` dans la transaction qui publie une archive retenue. Historique complet, index figé et sa base restent reconstruisibles après GC. La branche symboliquement sélectionnée est protégée même si une autre a le même ID. Un checkout/switch actif bloque les mutations. Fichiers et sélection restent inchangés.

Tout index, propre ou périmé, exige `--archive-index` ; sinon le plan indique `staging_present`. Le CLI exige `--expect` avec l’ID exact de tête ; `--expect-index` est optionnel, sinon l’index courant est capturé sous verrou. L’API C++ attend un index optionnel exact : null exige l’absence, un ID autorise ce seul snapshot. Une divergence bloque avant publication. Aucune version non fusionnée n’est implicitement abandonnée : tout l’historique est archivé sans condition d’ancêtre.

Le [retrait protégé par sauvegarde](doc:archive-retention) peut retirer plus tard tout le groupe `archive/branches/ID` seulement si descripteur, tête et index sont couverts par une sauvegarde indépendante vérifiée. Cela ne supprime ni branche recréée du même nom ni objets par GC automatique.

Le plan indique `branch`, `archive` proposé, `archive_existing`, `archive_index`, `ready`, `objects_verified`, `blockers`. Plan prêt : code 0 ; bloqué : code 1 avec JSON ; arguments/métadonnées invalides : diagnostic. L’application ajoute `state: deleted`, vérifie toujours toute la fermeture avant l’enregistrement ; les champs précédents décrivent ses entrées. Seul `--verify` contrôle les payloads dans un plan.

L’identité déterministe dépend du nom, de la tête et de l’index. Répéter le même état réutilise l’archive complète ; un groupe partiel/conflictuel bloque. Avant publication, erreur ou annulation conserve les références mais peut laisser des métadonnées non référencées. Inspecter avant de réessayer. Aucun journal de fichiers n’est nécessaire.

Le chunk descripteur JSON canonique contient `version: 1`, `type: native-branch-archive`, `name`, `revision`, `index` nullable. Le groupe retient `record` (chunk), `target` (révision/historique), `index` optionnel (snapshot/base) sous `archive/branches/ARCHIVE_ID/`.

Le navigateur contrôle identité, groupe exact et index lié à la branche. `branch-archives` utilise pages triées par ID, curseur exclusif `next_after`, `has_more`, `objects_verified`, limite 1..1000. Chaque ligne donne `archive`, nom original et résumé tête/index sans `current`. `branch-archive` retourne une ligne dans `archive`. `--verify` audite les racines affichées. Bases périmées ou initialement non nées restent exactes. Aucune expiration automatique ; retirer génériquement ces racines libère leur rétention. `native archives` reste dédié aux archives de révisions comme reset/amend/checkout. Les formats d’objets et stockage ne changent pas.

```text
syrnck native branch-delete-plan feature/topic --archive-index --verify
syrnck native branch-delete feature/topic --expect TIP_ID --archive-index --expect-index INDEX_ID
syrnck native branch-archives --limit 100 --verify
syrnck native branch-archive ARCHIVE_ID --verify
```


```cpp
auto deletion = store.plan_delete_native_branch("feature", true, true, operation);
auto removed = store.delete_native_branch("feature", expected_tip, expected_index, operation);
auto saved = store.native_branch_archive(removed["archive"].get<std::string>(), true, operation);
```


## Restaurer une branche archivée {#restore-an-archived-branch}

La restauration recrée tête et index dans une transaction. Les deux slots doivent être absents, même si une branche existante porte le même ID. Un index orphelin ou checkout/switch actif bloque. L’archive reste retenue. Fichiers et sélection symbolique sont inchangés ; une sélection non née du nom restauré résout sa tête ensuite. Utiliser explicitement switch/checkout pour les fichiers.

Sans `--as`, nom et ID d’index originaux sont restaurés. Avec un autre nom, seul le message interne de liaison de l’index change : fichiers, modes, timestamp et parents restent identiques. Le nouvel ID est déterministe, éventuellement non stocké avant application. Un index non né conserve ses parents vides ; un index périmé le reste. Inspecter `native staged --branch NAME` puis utiliser le [rebase d’index](doc:native-stage-rebase) si nécessaire.

Rapport : `archive`, `original_name`, `name`, `revision`, `previous_index`, `index`, `index_rewritten`, `index_base`, `stale_index`, `current`, `ready`, `objects_verified`, `blockers`. Plan prêt : 0 ; bloqué : 1 avec JSON. L’application vérifie toute l’archive avant tout index réécrit ou publication et ajoute `state: restored`. Le plan ne lit les données qu’avec `--verify`.

La transaction garde le groupe d’archive, les absences des destinations et l’objet HEAD attendus. Les racines sources restent intactes. Avant publication, annulation/observer préserve l’ancienne table ; un index candidat peut rester non référencé. Inspecter archive et destination avant nouvelle tentative : une destination déjà complétée est une collision. Les versions archivées survivent au GC.

```text
syrnck native branch-restore-plan ARCHIVE_ID --verify
syrnck native branch-restore ARCHIVE_ID
syrnck native branch-restore-plan ARCHIVE_ID --as recovered/topic --verify
syrnck native branch-restore ARCHIVE_ID --as recovered/topic
```


```cpp
auto preview = store.plan_restore_native_branch(archive_id, "recovered", true, operation);
auto restored = store.restore_native_branch(archive_id, "recovered", operation);
```


## Défauts CLI et identité du workspace {#cli-defaults-and-workspace-identity}

Sans `--branch`, la branche courante ou cible détachée est utilisée pour commit simple/figé, add, staged, reset, status simple/combiné, checkout-plan, checkout, log et détachement d’historique. Une branche explicite n’altère pas HEAD. `status --revision` sélectionne le snapshot demandé.

Les arguments snapshot acceptent [tags et sélecteurs](doc:native-tags). Le switch ordinaire prend un nom littéral de branche ; `native checkout tags/NAME` déplace la cible sélectionnée et archive sa tête. Utiliser `native switch --detach tags/NAME` pour détacher HEAD.

`native head` retourne des clés anglaises : `version` (1), `branch`, `revision` (null si non née), `explicit`, `directory` (racine canonique ou null pour HEAD implicite). Le descripteur résout la branche dynamiquement sans réécriture à chaque commit. Une suppression générique de sa tête laisse une sélection non née.

Avec HEAD explicite, `--directory` (défaut `.`) doit résoudre la même racine canonique. Un Store ne sélectionne qu’un workspace. Workspaces liés indépendants, déplacement et switch vers destination non née restent à réaliser. Les méthodes C++ à branche explicite ne résolvent pas implicitement HEAD ; l’appelant peut utiliser `native_target()` pour une branche ou la cible détachée opaque. Voir [HEAD détaché](doc:native-detached), dont `native head` indique `branch: null`.

## Préparation, publication et récupération {#preflight-publication-and-recovery}

`native switch-plan NAME [--verify]` est en lecture seule, basé sur le preflight checkout de la branche courante vers la tête destination. Il ajoute `operation: switch`, `destination_branch`, `head_object` (ID ou null), `destination_staging_snapshot`. Un index sur l’une des branches bloque, même propre ; côté destination : `target_staging_present`. Un plan bloqué complet donne JSON/code 1 ; arguments ou objets requis invalides ne donnent pas de rapport.

La destination doit exister et être une révision. Tous les fichiers suivis doivent correspondre par hash complet. Collisions non suivies/ignorées, liens, alias portables et transitions suivent le checkout. Aucun force, écrasement ou reset d’index automatique. Un switch propre vers la branche déjà sélectionnée est un no-op en lecture seule ; deux branches au même ID publient néanmoins une nouvelle sélection.

La transaction vérifie têtes source/destination, slots d’index et HEAD. Elle audite les fermetures et reconstruit les cibles ; des déplacements exclusifs préservent les originaux. La transaction finale publie HEAD avec journal validé, archive source, rétention cible et reçu sans déplacer les têtes. Avant ce point, HEAD garde l’ancienne sélection même si des fichiers ont changé.

Avant publication, abort restaure les originaux et HEAD ; resume termine la nouvelle sélection. Après publication, seule la fin du nettoyage est possible. Une mutation générique des branches/index/HEAD fait refuser la récupération. Ouvrir le Store ne modifie jamais automatiquement les fichiers. Les déplacements sont séquentiels ; arrêter les éditeurs. Cibles préparées et sauvegardes sont en clair même avec stockage chiffré ; les limites du checkout s’appliquent.

```text
syrnck native checkout-status
syrnck native checkout-recover --progress
syrnck native checkout-recover --abort --progress
syrnck native checkout-status --id TRANSACTION_ID
```


## Représentation et API C++ {#representation-and-c-api}

`workspace/HEAD` retient un chunk JSON canonique : `version: 1`, `type: native-head`, `directory`, `branch`. Compression, chiffrement et packs ordinaires s’appliquent sans nouveau format. Aucun ID de révision figé : les références de branche retiennent leurs fermetures.

Le journal switch v2 ajoute `destination_branch`, `previous_head` nullable et `next_head` à l’identité hashée. Les journaux/reçus v1 restent lisibles ; GC bloqué pendant une transaction. Les anciens descripteurs peuvent être collectés après switch ; les reçus terminés restent consultables sans eux. Journaux actifs et HEAD sont locaux : ne pas les éditer ni les répliquer comme des racines de livraison. Employer un moteur récent après activation. L’ABI C et Go n’exposent pas encore les opérations de branche.

```cpp
auto head = store.native_head();
auto branches = store.native_branches({}, 100, true, operation);
auto created = store.create_native_branch("feature", revision_id, operation);
auto plan = store.plan_switch(project, "feature", true, operation);
auto switched = store.switch_directory(project, "feature", operation);
```
