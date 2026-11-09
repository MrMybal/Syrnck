---
id: native-file-history
title: Renommages et historique de fichiers
description: Renommages exacts ou modifiés, lignée et fusions tenant compte des nouveaux noms.
section: Versionnage
order: 250
parent: native-history
sourceRevision: 2
---

# Renommages et historique par premier parent {#exact-renames-and-first-parent-file-history}

Les API de `syrnck/history.hpp` comparent les renommages exacts et parcourent la lignée à partir des manifestes vérifiés. Par défaut elles ne reconstruisent pas les payloads. La similarité explicite ajoute les renommages modifiés et peut lire les petits fichiers. Ces requêtes ne lisent pas le workspace et ne publient aucune référence. Une comparaison de métadonnées ne remplace pas un audit complet d’intégrité.

```text
syrnck native diff SOURCE TARGET --renames
syrnck native log-file HEAD Content/Map.uasset
syrnck native log-file HEAD Content/Map.uasset --limit 25 --max-revisions 1000
syrnck native log-file HEAD Content/Map.uasset --no-follow
```


## Comparer des renommages exacts {#exact-rename-comparison}

`native diff --renames` compare les snapshots puis associe un chemin supprimé à un ajouté si taille et hash BLAKE3 complet correspondent exactement, avec correspondance unique dans les deux sens. Cela couvre déplacements, casse/orthographe Unicode et découpages/compressions différents. Le changement de mode exécutable reste distinct.

Une ligne `status: renamed` contient `path` cible, `old_path`, `similarity: 100`, métadonnées source/cible et indicateurs séparés de contenu/mode/manifeste. Le compteur renamed augmente tandis que les ajouts/suppressions associés diminuent. Sans option, le diff conserve les ajouts/suppressions habituels.

Plusieurs correspondances de contenu identique restent des ajouts/suppressions avec `rename_ambiguous` et compteurs d’ambiguïté ; une copie inchangée existante ne devient pas un renommage. Copies et permutations de chemins existants ne sont pas inférées ; les renommages modifiés et leur rejeu demandent les options ci-dessous. Le JSON reste borné à 16 Mio.

## Suivre une lignée {#follow-a-file-lineage}

`native log-file REVISION PATH` suit le premier parent et rend les changements du fichier : contenu, mode, représentation et suppression. Il suit en arrière les renommages exacts uniques et saute commits sans rapport ou vides. Un chemin déjà supprimé/renommé au départ peut montrer son passé. Chaque entrée porte révision, chemin à cette révision, aperçu de message borné, date, parents et changement de snapshot.

Le premier ajout termine la lignée. Une origine ambiguë s’arrête avec `stop_reason: ambiguous_rename`, sans choix arbitraire. `--no-follow` s’arrête aux ajouts du chemin demandé, y compris ceux provenant d’un renommage. Une fusion est comparée à son premier parent ; les autres parents sont décrits mais non parcourus. Ce n’est ni un blame multi-parent ni un moteur de recherche historique général.

Défaut 100 changements, plage 1..1000 ; budget distinct de 1000 révisions inspectées, plage 1..10000. Une page incomplète rend `next_revision` et `next_path`, à reprendre ensemble même après renommage. `complete` et `stop_reason` distinguent added, ambiguous_rename, root, page_limit, traversal_limit. Ces requêtes ne créent pas de pins ; les racines normales doivent protéger les versions contre un retrait/GC administratif entre appels.

```text
syrnck native log-file NEXT_REVISION NEXT_PATH --limit 25
```

## Renommages modifiés et similarité {#edited-renames}

```text
syrnck native diff BASE HEAD --rename-similarity 50
syrnck native log-file HEAD Source/New.cpp --rename-similarity 50
syrnck native blame HEAD Source/New.cpp --rename-similarity 50
```

La surcharge à cinq arguments de `native_diff_renames` accepte
`NativeRenameOptions` et `OperationOptions`. Les options d'historique/blame ont
`renames` facultatif. Le défaut reste exact ; `--rename-similarity N` active la
similarité explicitement.

Les correspondances uniques de fichier complet passent en premier. Les
ambiguïtés de contenu identique ne sont pas recalculées. Les chemins supprimés/
ajoutés restants utilisent les identités communes de blocs, pondérées par leurs
octets. Le texte UTF-8 vérifié de 256 Kio au plus utilise aussi des fragments
délimités par les lignes et limités à 64 octets, reconnaissant une édition dans un
bloc. Les grands fichiers utilisent les blocs ; un découpage différent peut
réduire la similarité binaire. Les petits candidats binaires peuvent être lus
pour déterminer s'ils sont textuels. Les grands payloads ne sont pas reconstruits.

Le score divise les octets communs par la taille du plus grand fichier, en
retenant le meilleur score de blocs/fragments textuels. Les répétitions ne
comptent que jusqu'à leur poids sur chaque côté. Un index inversé évite toutes
les paires systématiques ; les entrées très répétitives peuvent encore épuiser
le travail. Les meilleures fractions sont comparées exactement avant arrondi.
Seuls les meilleurs choix uniques et réciproques atteignant N (1..100) sont
associés. Les égalités restent ambiguës. Cette heuristique ne prouve pas
l'intention et ne tient pas compte de l'ordre : même 100 peut accompagner
`content_changed: true`. Pour exiger l'identité, garder le mode exact `--renames`.

| Option | Défaut | Plage acceptée |
| --- | --- | --- |
| `--rename-max-candidates` | 4096 manifestes candidats distincts | 1..16384 |
| `--rename-max-work` | 1000000 visites de fragments/index | 1..16000000 |
| `--rename-max-input-bytes` | 67108864 octets d'entrées supplémentaires | 1024..268435456 |

Les budgets demandent `--rename-similarity`. Ils couvrent les décodages et
reconstructions supplémentaires, pas toutes les allocations ni les lectures
ordinaires des snapshots. `rename_detection` expose la consommation. L'historique
partage un cache/budget sur sa page ; répéter les options avec
`next_revision`/`next_path`. `--no-follow` est incompatible. Le blame compte ces
lectures dans son budget global. Diff/historique limités échouent ; le blame
limité rend une attribution vide.

## Fusions et rejeu avec renommages {#rename-merges}

```text
syrnck native merge-plan feature --rename-similarity 50 --text-merge
syrnck native cherry-pick-plan COMMIT --rename-similarity 50 --text-merge
syrnck native revert-plan COMMIT --rename-similarity 50 --text-merge
syrnck native rebase-plan BASE --onto upstream --rename-similarity 50 --text-merge
syrnck native rebase-index-plan --rename-similarity 50 --text-merge
```

Répéter les options pour appliquer. `NativeMergeOptions`, `NativeReplayOptions`,
`NativeRebaseOptions` et `NativeStageRebaseOptions` ont `renames` facultatif.
Une modification sous l'ancien nom suit un renommage unique, y compris le mode.
Deux renommages vers la même destination utilisent la même base. `--text-merge`
compose le texte compatible ; les binaires sont sélectionnés comme manifestes
complets ou restent en conflit.

Les plans exposent `renames` avec chemins original/current/incoming et preuves de
similarité. Destinations différentes (`rename_rename`), renommage/suppression
(`rename_delete`/`delete_rename`) et destination occupée (`rename_add`) demandent
des choix explicites. Résoudre chaque chemin avec le contexte habituel ; le nom
d’origine est inclus pour pouvoir le restaurer avec un choix base. Les choix
de contenu à la destination utilisent ses descripteurs alignés. Les gardes de
casse et fichier/dossier restent actives. Les copies de conflit conservent les
contenus concurrents ; elles ne règlent pas les choix structurels et ne
réutilisent pas les noms d'origine.

Le rebase partage un cache/budget sur la séquence, y compris les textes virtuels ;
le second passage doit reproduire la même cible. Le rebase d'index conserve les
représentations préparées et archive l'index précédent sans toucher au workspace.
L'application vérifie les historiques complets et garde les contrôles des
signatures, références et checkout. Après arrêt de processus, un checkout préparé
reprend sa cible figée sans déduire le renommage à nouveau. Le GC reste bloqué
pendant le checkout ; après reprise, les racines protègent encore les versions.
