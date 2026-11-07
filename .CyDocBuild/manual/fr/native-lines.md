---
id: native-lines
title: Sélection de lignes textuelles
description: Guide : Sélection de lignes textuelles.
section: Versionnage
order: 290
parent: native-history
sourceRevision: 1
---

# Sélection individuelle des lignes {#native-individual-text-line-selection}

Préparer ou retirer des lignes ajoutées/supprimées à l’intérieur d’un hunk sans écrire le workspace. Les commandes de [hunks entiers](doc:native-hunks) restent disponibles. Choisir les numéros dans le bon aperçu frais avec les mêmes options : les plans add et reset ont des identités et parfois des numérotations différentes. La langue ne modifie ni clés, ni nombres, ni IDs.

```text
syrnck native add-lines-plan Source/Player.cpp
syrnck native add-lines Source/Player.cpp --expect-plan PLAN_ID --lines 1,3
syrnck native reset-lines-plan Source/Player.cpp
syrnck native reset-lines Source/Player.cpp --expect-plan PLAN_ID --lines 2,4
```


## Numérotation des changements {#numbered-change-rows}

Chaque plan expose selection_unit line, change_count et diff.hunks numérotés. Chaque ligne add/remove possède en plus un change_number global à partir de 1 dans l’ordre des hunks ; le contexte n’a pas de numéro. --lines désigne ces changements, pas les lignes du fichier ou positions locales. Liste non vide, unique, positive, en plage, au plus 100000 numéros. Au-delà de 100000 changements, tout le plan bloque avec selection_limit.

Dans un remplacement, retraits et ajouts sont indépendants. Avec les quatre lignes d’exemple, 2,4 remplace old B par new B en gardant old A ; 3 seul garde les deux anciennes lignes puis ajoute new A ; 1 seul retire old A sans remplacement. L’ordre du diff fixe les insertions ; ni réordonnancement ni édition libre. Aucune validation de syntaxe/compilation du résultat.

Pour reset, la même direction base-vers-index numérote ce qui est retiré. Les autres changements restent préparés ; reconstruire depuis la base et les lignes conservées évite les offsets déplacés. Tout retirer rétablit le contenu de base tout en gardant mode, autres fichiers et racine staging. Obtenir ensuite un nouvel aperçu.

```text
change_number  kind    text
1              remove old A
2              remove old B
3              add    new A
4              add    new B
```


## Octets exacts et refus {#exact-bytes-and-selection-refusal}

UTF-8, LF/CRLF et newline finale restent exacts. Aucune newline n’est inventée. Une sélection qui ajouterait une ligne après une ligne conservée non terminée est refusée avant ingestion. Dans l’exemple, sélectionner seulement l’ajout concaténerait old et new ; sélectionner retrait et ajout pour remplacer. Le reset partiel équivalent est aussi refusé.

Un plan prêt signifie comparaison éligible, pas validité de toutes les combinaisons. Certaines sélections échouent sur ce raccord ou le budget du résultat, qui peut dépasser les deux entrées en gardant des suppressions non sélectionnées et insérant des ajouts.

```text
source: old (no final LF)
target: new (with final LF)
```


## Plans, stockage et rétention {#plans-storage-and-retention}

Le plan add operation stage-lines compare index/tête figé au fichier observé, avec --directory et les protections du lecteur ciblé : ignores, namespace, liens, tailles et mutations observées. Les métadonnées sont revérifiées après ingestion et après le dernier observateur avant publication.

Le reset operation reset-lines compare base originale et index figé, sans --directory ni contenu de travail. Les chemins disque peuvent être absents, sales, ignorés ou des dossiers. Le fichier doit exister des deux côtés stockés.

Les deux formes exigent du texte existant, un index frais s’il existe et aucun checkout actif. Nouveaux fichiers, changements complets de présence, binaire/limites, mode seul et archives suivent les restrictions de hunks. Une branche initiale permet add sur un fichier déjà dans son index initial ; ses ajouts initiaux demandent reset par fichier.

Branche, progression/annulation et cinq options de [diff](doc:text-diff) s’appliquent. Numérotation/contexte sont supplémentaires au budget de diff, avec rapport total de 16 Mio. Changement d’options, diff, branche/index/base, état prêt ou contenu observé impose un nouveau --expect-plan ; un plan hunk n’autorise pas une application ligne. Plan prêt : 0 ; bloqué : 1 avec JSON ; sélection invalide : erreur sans résultat.

La transaction vérifiée conserve voisins et mode source, publie l’index avec archive/stage-index/OLD_INDEX_ID s’il existait et retient toute l’ancienne préparation/base après GC et commit. Annulation, observateur ou fermeture invalide gardent l’ancien état et peuvent laisser des objets complets inaccessibles ; aucun callback après publication. Voir la [récupération des sélections](doc:native-hunks#atomic-retention-and-recovery).

C++ expose plan_stage_lines, stage_lines, plan_reset_lines et reset_lines avec les paramètres des versions hunks. Aucun format logique/chiffrement ne change ; recompiler avec les en-têtes correspondants.
