---
id: performance
title: Performance et budgets de ressources
description: Performance et budgets de ressources
section: Stockage
order: 90
parent: storage
sourceRevision: 1
---

## Choisir les budgets du corpus {#budgets}

Cache décodé : 64 Mio ; empreintes : 8192 entrées ; pages d’index : 4 Mio. Ces budgets indépendants ne bornent pas toute la RAM. L’ingestion utilise un worker et 32 Mio bruts en attente par défaut ; les workers ajoutent codecs/sorties. Requêtes/compression réseau ont leurs bornes distinctes. Voir [réglages](doc:settings), [index](doc:indexes), [transferts parallèles](doc:parallel-transfers), [compression](doc:network-compression).

Objets et manifestes/révisions plats : 16 Mio. Catalogue : 1024 packs, 1 048 576 entrées par pack, 4 194 304 au total. Les plafonds ne prouvent pas une échelle validée. Des assets Unreal réécrits/compressés/chiffrés en amont peuvent peu se dédupliquer.

## Interpréter les compteurs {#measurement}

`io_stats` compte lectures/octets d’enveloppes réussis, décodages et hits ; les champs mémoire d’index couvrent capacités utiles de répertoire/filtre/pages sans allocateurs ni cache système. Le GC expose marques empaquetées, clés vivantes isolées et pic de pile. Aucun ne mesure le RSS global.

L’ouverture valide linéairement les index complets. Audits/GC/compaction en flux réduisent les inventaires empaquetés, mais clés isolées, métadonnées, piles, codecs et caches coûtent toujours. Checkout reconstruit les fichiers changés complets et utilise espace privé/validation complète. Watch cible des sous-arbres entre audits complets. Rapports synthétiques et scripts de reproduction restent des données développeur hors manuel ; leurs durées ne promettent pas un débit universel.
