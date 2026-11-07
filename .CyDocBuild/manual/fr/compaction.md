---
id: compaction
title: Empaquetage et compaction en flux
description: Guide : Empaquetage et compaction en flux.
section: Stockage
order: 70
parent: storage
sourceRevision: 1
---

# Empaquetage et compaction en flux {#streaming-packing-and-compaction}

`syrnck pack` ajoute des packs pour les nouveaux objets isolés ; `pack --compact` regroupe tous les objets actifs, même inaccessibles. Le `gc` retire les objets non retenus. Les manifestes complets et leurs blocs sont préservés sans chaîne de deltas binaires.

Les deux opérations commencent par un audit complet, hashes de fichiers et doublons isolés/empaquetés compris. Les enveloppes sont conservées exactement, sans recompression ni changement de clé ; les lectures privilégient la copie isolée.

## Entrées triées et deux passages {#ordered-input-and-two-passes}

Chaque index immuable est déjà trié. La compaction fusionne les index avec une tête et une page possédée de 256 entrées par pack. Les recherches imbriquées n’invalident pas les pages, même sans cache ; la session bornée de handles se ferme à épuisement, annulation ou erreur.

Seules les clés des objets exclusivement isolés sont collectées et triées : 33 octets utiles par clé avant capacité/allocation. Les doublons empaquetés n’ajoutent aucune clé. Le pack incrémental consomme cette séquence ; la compaction la fusionne avec les index sans liste complète d’IDs empaquetés.

Le premier passage vérifie ordre strict, nombres, longueurs d’enveloppes, budgets et capacité. Il garde nombre, taille et digest BLAKE3 de séquence par pack de sortie. Une capacité insuffisante ou source malformée échoue avant lecture des payloads ou création des sorties.

Le second passage repart d’un curseur neuf et écrit les packs prévus. Le digest lie IDs typés et longueurs au plan ; tout changement d’ID, taille ou nombre, ou erreur source/codec, arrête le lot. Les fichiers déjà terminés restent inactifs jusqu’au nettoyage, sans remplacer le catalogue.

Toutes les nouvelles enveloppes sont relues et vérifiées par pages avant une publication atomique unique. Les curseurs et sessions de l’ancien catalogue sont fermés avant publication. Un nouveau parcours des objets isolés retire les copies confirmées dans le catalogue publié ; les packs générés inactifs sont retirés en dernier. Voir la [reprise](doc:packs).

## Mémoire et limites {#memory-and-remaining-limits}

Planification, partitions, relecture et nettoyage ne gardent aucune liste complète d’IDs empaquetés. La fusion conserve au plus 11 520 octets de données d’entrées par pack, soit 11,25 Mio à 1 024 packs. Tas, curseurs, snapshots, répertoires/filtres, caches, enveloppes, codecs et audit initial s’ajoutent.

Le tri des clés isolées, leurs marques d’audit et les métadonnées restent proportionnels. `objects()` et `closure()` matérialisent leurs résultats. Il n’existe ni garantie globale de mémoire constante, ni relèvement des plafonds du catalogue, ni maintenance concurrente en ligne. Le verrou exclusif couvre l’opération. Formats, IDs, options et ABI C restent identiques.

## Préparer la maintenance {#tests-and-reproduction}

Prévoir la place d’une génération complète avant retrait des anciens packs. Garder une sauvegarde vérifiée et examiner le résultat avant le GC destructif. Voir la [publication et reprise](doc:packs#publication-et-reprise).

## Limites de performance {#windows-x64-measurement-4-november-2026}

Le débit dépend de la disposition des objets, des codecs, du stockage et des caches. Les plans en flux réduisent les inventaires d’IDs sans garantir un débit supérieur. Voir les [budgets](doc:performance).
