---
id: native-file-history
title: Renommages exacts et historique de fichiers
description: Guide : Renommages exacts et historique de fichiers.
section: Versionnage
order: 250
parent: native-history
sourceRevision: 1
---

# Renommages exacts et historique par premier parent {#exact-renames-and-first-parent-file-history}

Les API de `syrnck/history.hpp` comparent les renommages exacts et parcourent l’historique de fichiers en lisant seulement les métadonnées de manifestes vérifiées. Elles ne lisent pas le workspace, ne changent pas les références et ne reconstruisent pas les payloads. Elles ne remplacent pas un audit complet d’intégrité.

```text
syrnck native diff SOURCE TARGET --renames
syrnck native log-file HEAD Content/Map.uasset
syrnck native log-file HEAD Content/Map.uasset --limit 25 --max-revisions 1000
syrnck native log-file HEAD Content/Map.uasset --no-follow
```


## Comparer des renommages exacts {#exact-rename-comparison}

`native diff --renames` compare les snapshots puis associe un chemin supprimé à un ajouté si taille et hash BLAKE3 complet correspondent exactement, avec correspondance unique dans les deux sens. Cela couvre déplacements, casse/orthographe Unicode et découpages/compressions différents. Le changement de mode exécutable reste distinct.

Une ligne `status: renamed` contient `path` cible, `old_path`, `similarity: 100`, métadonnées source/cible et indicateurs séparés de contenu/mode/manifeste. Le compteur renamed augmente tandis que les ajouts/suppressions associés diminuent. Sans option, le diff conserve les ajouts/suppressions habituels.

Plusieurs correspondances de contenu identique restent des ajouts/suppressions avec `rename_ambiguous` et compteurs d’ambiguïté ; une copie inchangée existante ne devient pas un renommage. Renommages édités, heuristiques de similarité, copies, permutations de chemins existants et résolutions de merge/rebase tenant compte des renommages ne sont pas inférés. Le JSON reste borné à 16 Mio.

## Suivre une lignée {#follow-a-file-lineage}

`native log-file REVISION PATH` suit le premier parent et rend les changements du fichier : contenu, mode, représentation et suppression. Il suit en arrière les renommages exacts uniques et saute commits sans rapport ou vides. Un chemin déjà supprimé/renommé au départ peut montrer son passé. Chaque entrée porte révision, chemin à cette révision, aperçu de message borné, date, parents et changement de snapshot.

Le premier ajout termine la lignée. Une origine ambiguë s’arrête avec `stop_reason: ambiguous_rename`, sans choix arbitraire. `--no-follow` s’arrête aux ajouts du chemin demandé, y compris ceux provenant d’un renommage. Une fusion est comparée à son premier parent ; les autres parents sont décrits mais non parcourus. Ce n’est ni un blame multi-parent ni un moteur de recherche historique général.

Défaut 100 changements, plage 1..1000 ; budget distinct de 1000 révisions inspectées, plage 1..10000. Une page incomplète rend `next_revision` et `next_path`, à reprendre ensemble même après renommage. `complete` et `stop_reason` distinguent added, ambiguous_rename, root, page_limit, traversal_limit. Ces requêtes ne créent pas de pins ; les racines normales doivent protéger les versions contre un retrait/GC administratif entre appels.

```text
syrnck native log-file NEXT_REVISION NEXT_PATH --limit 25
```
