---
id: native-archives
title: Archives de l’historique natif
description: Guide : Archives de l’historique natif.
section: Versionnage
order: 200
parent: native-history
sourceRevision: 1
---

# Archives de l’historique natif {#native-history-archives}

Les mutations d’historique conservent anciennes révisions et préparations comme racines ordinaires du GC. L’inventaire les consulte sans modifier références, objets ou fichiers de travail, y compris amend, checkout et détachement d’historique.

`reset` conserve l’ancienne tête ; `reset-index` l’ancien index et sa base ; `rebase-index` l’index avant son rejeu ; `stage-index` l’index avant sélection de hunks/lignes ou patch externe ; `squash` toute la chaîne d’origine ; `amend` l’ancienne tête et ses parents ; `checkout` la source du checkout terminé ; `detached` la tête d’origine sous `archive/OLD_ID`. Pour les autres catégories actuellement reconnues, consulter l’inventaire et ses filtres.

Les groupes de tags relèvent de `native tag-archives` / `tag-archive` / `tag-restore` ; ceux de branches de `native branch-archives` / `branch-archive`. Ils ne figurent pas dans cet inventaire simple. Les namespaces inconnus/personnalisés sont exclus ; `refs` reste l’inventaire générique complet. Les noms reconnus, types révision et correspondances d’ID sont contrôlés. Un nom de checkout désigne une transaction plutôt que sa révision cible. Une archive reconnue corrompue provoque une erreur.

```text
syrnck native archives --limit 100
syrnck native archives --kind reset-index --verify
syrnck native archives --after archive/reset/OLD_ID --limit 100
syrnck native archive-show archive/squash/OLD_ID --verify
```


## Pagination et vérification {#pages-and-verification}

`--kind` filtre une catégorie ; `--limit` accepte 1..1000, avec 100 par défaut. Ordre : nom complet de référence. `next_after` est le curseur exclusif lorsque `has_more` vaut true, sinon null. Il reste utilisable après retrait de sa référence, mais sa catégorie doit correspondre à un filtre explicite. Chaque page reflète les références actuelles ; aucune vue persistante ne fige plusieurs appels.

Chaque ligne contient `reference`, `kind`, l’ID exact `revision`, nombres de fichiers/parents, date de révision et aperçu du message limité à 256 octets UTF-8 sans couper un caractère. `message_bytes` et `message_truncated` décrivent le message complet. La date est celle de la révision, pas celle de l’archivage. `archive-show` rend la même ligne pour une référence ; `inspect revision ID` fournit toutes les métadonnées.

Par défaut, seules références et révisions sont lues ; les blocs peuvent manquer. `--verify` audite la fermeture complète et les hashes de fichiers des lignes affichées uniquement. Les octets conservés absents/corrompus font échouer la requête. Progression/annulation et diagnostics bilingues sont disponibles, JSON anglais et callbacks non réentrants.

Sortie bornée à 16 Mio. La table des références est encore chargée intégralement ; la pagination borne lignes et lectures de révisions, pas sa mémoire. Les stockages en clair/chiffrés, isolés/empaquetés utilisent les mêmes racines, sans changement de format ou politique.

## Récupérer les versions conservées {#recovering-stored-versions}

Exporter l’ID `revision` d’une ligne ou créer une branche de récupération. Les règles de propreté et de récupération de checkout/switch s’appliquent ensuite. Un index archivé s’exporte aussi pour retrouver ses octets figés ; l’inventaire ne restaure pas automatiquement un couple branche/index et ne change pas les modifications de travail. Les fichiers jamais importés sont hors rétention.

Les archives n’expirent pas automatiquement. Le [retrait protégé par sauvegarde](doc:archive-retention) gère leur nettoyage explicite ; retirer la dernière racine puis lancer un GC destructif peut supprimer leurs objets exclusifs. L’inventaire seul ne promet aucun gain disque.

API disponibles en C++ et CLI, pas encore dans l’historique C/Go. Les index remplacés par sélection textuelle gardent leur fermeture et leur base sous `archive/stage-index/ID`, filtrables avec `--kind stage-index`. Voir la [sélection de hunks](doc:native-hunks).

```text
syrnck native export REVISION_ID recovered-version
syrnck native branch recovered REVISION_ID
syrnck native switch-plan recovered --verify
syrnck native switch recovered
```


```cpp
auto page = store.native_archives("reset-index", {}, 100, true, operation);
auto archive = store.native_archive("archive/reset/" + old_id, true, operation);
```
