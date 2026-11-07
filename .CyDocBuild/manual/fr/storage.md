---
id: storage
title: Stockage et reconstruction
description: Stockage et reconstruction
section: Stockage
order: 100
sourceRevision: 1
---

## Objets et versions complètes {#objects}

Un ID BLAKE3 typé adresse un chunk, manifeste ou révision. Le manifeste conserve la liste complète ordonnée des blocs, longueur, hash complet et découpage. Une révision contient un snapshot complet des chemins et ses parents. Partager des blocs ne crée aucune dépendance à rejouer une ancienne version de fichier.

`put` conserve le manifeste sous `files/ID` ; `get` le restaure exclusivement vers un nouveau chemin. `inspect`, `compare`, `diff` et `refs` consultent les métadonnées. Voir la [comparaison](doc:file-comparison) et les [plages vérifiées](doc:file-ranges). La reconstruction complète vérifie chaque bloc et le hash du fichier.

## Rétention avant collecte {#retention}

Branches, tags, index, archives et pins conservent leurs dépendances, parents de révisions compris. Retirer un commit visible ne supprime pas nécessairement ses blocs. Inspecter d’abord les protections, retirer ensuite explicitement les racines avec une [sauvegarde vérifiée](doc:archive-retention), puis seulement envisager le GC destructif. Le GC prévisualise par défaut et refuse toute dépendance conservée absente/corrompue avant suppression.

Les [racines de checkout](doc:checkout-retention), [journaux de transfert](doc:selective-retention) et baux actifs ont leurs règles propres. Sauvegarde et restauration sont des snapshots logiques, pas des copies de fichiers de travail non commités. Stabiliser les fichiers modifiés par d’autres programmes pendant leur analyse.

## Stockage physique et budgets {#physical}

Les nouveaux dépôts stockent des objets isolés v1. L’empaquetage explicite sélectionne le stockage physique v2/v3 tout en gardant les IDs et enveloppes logiques v1. Voir [packs](doc:packs), [index paginés](doc:indexes), [compaction](doc:compaction), [audits](doc:audit) et [annulation de maintenance](doc:maintenance).

Défauts FastCDC : minimum 256 Kio, cible 1 Mio, maximum 4 Mio ; découpage fixe optionnel. Les objets décompressés et manifestes/révisions plats sont limités à 16 Mio. Cache décodé : 64 Mio ; empreintes vérifiées : 8192 entrées ; pages d’index : 4 Mio. Ces budgets ne bornent pas toute la mémoire du processus. Les [préférences](doc:settings) contrôlent séparément workers d’ingestion et création des packs.
