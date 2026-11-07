---
id: native-hunks
title: Sélection de hunks textuels
description: Guide : Sélection de hunks textuels.
section: Versionnage
order: 280
parent: native-history
sourceRevision: 1
---

# Sélection partielle de hunks textuels {#native-partial-text-staging-and-reset}

Sélectionner des hunks numérotés d’un fichier existant sans modifier sa copie de travail, en complément de l’ajout par fichier/dossier. Les commandes acceptent branche, dossier, progression/annulation et les options de diff context/max-file-bytes/max-lines/max-work/max-output-bytes. JSON, IDs et numéros restent identiques en anglais/français. L’application rend du JSON, contrairement à l’ID discret de native add. Le budget du diff ne comprend pas numérotation et contexte ; le rapport complet reste sous 16 Mio.

```text
syrnck native add-hunks-plan Source/Player.cpp
syrnck native add-hunks Source/Player.cpp --expect-plan PLAN_ID --hunks 1,3
syrnck native staged
syrnck native diff-text --staged Source/Player.cpp --unified
syrnck native commit --staged -m "Selected changes"
```


## Examiner et lier la sélection {#review-and-bind-the-selection}

Le plan lit l’index figé ou la tête sans index et le fichier de travail observé. Les hunks portent des numéros à partir de 1. Le contexte détermine leur regroupement ; des éditions voisines dans un hunk sont prises ensemble. Les numéros appartiennent uniquement à ce plan. La [sélection de lignes](doc:native-lines) offre un autre niveau de choix ; aucune édition libre de patch ou invite interactive n’est proposée.

Rapport : diff de travail, operation stage-hunks, état branche/tête/index/base, archive optionnelle, text_options, ready, blockers, plan_id. Ce dernier est l’ID BLAKE3 du domaine chunk sur le JSON canonique avant ajout de plan_id, sans objet plan stocké. Il lie dossier/chemin/branche/manifeste/hash observé/mode disponible/hunks/options/disponibilité. La langue ne le change pas. Après application, le rapport garde l’ID d’aperçu, mais ses champs de résultat ne font plus partie de l’entrée de hash initiale.

L’application recalcule et exige l’ID exact attendu. Mutation du fichier, branche/index ou options impose un nouvel aperçu ; passer les mêmes options non par défaut aux deux commandes. Numéros positifs, uniques, présents, non vides et au plus 100000 ; leur ordre d’entrée ne change pas les octets. Refaire un aperçu après succès car les numéros restants peuvent changer.

Plan prêt : 0 ; bloqué : 1 avec JSON ; erreur ordinaire : diagnostic sans résultat. Blocages : checkout en attente, index périmé, diff binaire/limité, absence de changement textuel, changement de présence ou archive contradictoire. L’aperçu ne vérifie que ce fichier, pas toutes les autres données ; les fermetures complètes du remplacement et de l’archive sont vérifiées à la publication et peuvent encore bloquer.

## Portée et octets exacts {#scope-and-exact-bytes}

Source et fichier de travail doivent être des fichiers ordinaires existants ; fichiers vides, Unicode, LF/CRLF et absence de newline finale sont gérés. Binaire, nouveau fichier et suppression complète demandent native add ordinaire. Une branche initiale est possible si son index figé contient déjà le fichier. Un index périmé doit être rejoué ou réinitialisé explicitement.

Le candidat part de la table complète index/tête, ne remplace que le manifeste du chemin et conserve voisins/mode source. Les changements de mode et de découpage ne sont pas du texte sélectionnable. Hunks choisis copiés à l’octet ; régions non choisies gardées de source. Tous les hunks reproduisent le travail observé dans son budget. Un sous-ensemble peut être plus grand que les deux entrées en gardant des suppressions et ajoutant des lignes : refus avant ingestion s’il dépasse la limite.

Le lecteur ciblé applique ignores, orthographe exacte, protections namespace/liens/Store, bornes et détection des mutations observées. Voir le [diff de travail](doc:text-diff#compare-a-working-file). Aucun fichier de travail, tête de branche ou HEAD symbolique n’est modifié.

## Rétention atomique et récupération {#atomic-retention-and-recovery}

Les octets choisis sont ingérés avec CDC et préférences du dépôt. Le snapshot d’index v1 possède marqueur lié à la branche, date zéro et parent tête d’origine, ou aucun initialement. Aucun format/chiffrement ne change.

La transaction contrôle tête, index, HEAD symbolique et archive, puis publie staging/BRANCH et éventuellement archive/stage-index/OLD_INDEX_ID avec tout l’ancien index/base. Une archive exacte peut être réutilisée, une cible contradictoire est refusée. Inventaire, inspection et export permettent d’en récupérer les préparations.

Le chemin de travail est revérifié après ingestion puis immédiatement avant publication, après les observateurs. Annulation, erreur, mutation observée ou fermeture incomplète gardent les anciennes références ; des objets complets non référencés peuvent subsister. L’écriture atomique engagée peut finir malgré le stop ; aucun callback après. Après coupure, inspecter index/archive puis obtenir un nouveau plan : réutiliser un plan réussi ne l’applique pas silencieusement une seconde fois.

Pas d’isolation de snapshot filesystem ni protection absolue contre un écrivain hostile falsifiant les dates. Aucun journal workspace nécessaire puisqu’aucun fichier n’est écrit. Les archives restent après GC jusqu’au [retrait explicite protégé](doc:archive-retention). C++ utilise plan_stage_hunks et stage_hunks avec dossier, branche, chemin, limites, opération, plan attendu et numéros.

```text
syrnck native archives --kind stage-index --verify
syrnck native archive-show archive/stage-index/OLD_INDEX_ID --verify
syrnck native export OLD_INDEX_ID NEW_DIRECTORY
```


## Retirer des hunks préparés {#remove-prepared-hunks}

Le plan reset compare la base originale à l’index figé, dans le sens forward du diff staged. Les numéros désignent les changements à retirer ; les autres restent préparés. Le résultat part de la base plus les hunks conservés, sans rechercher des offsets déjà déplacés. Refaire le plan après chaque application.

Même branche, progression et cinq options textuelles, mais pas de --directory ni lecture du workspace, ignores ou chemins disque. Le fichier de travail peut être absent, sale, binaire ou devenu dossier sans effet. Le plan operation reset-hunks lie source_revision base, target_revision index, état/options/blocages, sans dossier/hash de travail. Même JSON bilingue, codes prêt/bloqué, validations, budgets et plan attendu que l’ajout partiel.

Il faut un index et le fichier dans base et index. Nouveaux fichiers, suppressions complètes et ajouts initiaux passent par native reset par fichier. Checkout actif, index périmé, binaire/limite, absence de texte changé et archive conflictuelle bloquent. Un mode seul n’est pas retirable en hunk. Tous les hunks rétablissent le contenu de base tout en gardant mode de l’index, voisins et racine staging.

Même transaction complète vérifiée et archive de l’ancien index, reconstructible après GC ou commit des changements restants. Annulation/exception/dépendance manquante gardent les références, avec objets orphelins possibles. Aucune revérification du workspace ou journal de fichiers n’est nécessaire. C++ utilise plan_reset_hunks et reset_hunks avec branche, chemin, limites, plan attendu et numéros.

```text
syrnck native reset-hunks-plan Source/Player.cpp
syrnck native reset-hunks Source/Player.cpp --expect-plan PLAN_ID --hunks 1,3
syrnck native diff-text --staged Source/Player.cpp --unified
```
