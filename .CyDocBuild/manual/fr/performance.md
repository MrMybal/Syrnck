---
id: performance
title: Performance et budgets de ressources
description: Performance et budgets de ressources
section: Stockage
order: 90
parent: storage
sourceRevision: 2
---

## Choisir les budgets du corpus {#budgets}

Cache décodé : 64 Mio ; empreintes : 8192 entrées ; pages d’index : 4 Mio. Ces budgets indépendants ne bornent pas toute la RAM. L’ingestion utilise un worker et 32 Mio bruts en attente par défaut ; les workers ajoutent codecs/sorties. Requêtes/compression réseau ont leurs bornes distinctes. Voir [réglages](doc:settings), [index](doc:indexes), [transferts parallèles](doc:parallel-transfers), [compression](doc:network-compression).

Objets et manifestes/révisions plats : 16 Mio. Catalogue : 1024 packs, 1 048 576 entrées par pack, 4 194 304 au total. Les plafonds ne prouvent pas une échelle validée. Des assets Unreal réécrits/compressés/chiffrés en amont peuvent peu se dédupliquer.

## Interpréter les compteurs {#measurement}

`io_stats` compte lectures/octets d’enveloppes réussis, décodages et hits ; les champs mémoire d’index couvrent capacités utiles de répertoire/filtre/pages sans allocateurs ni cache système. Le GC expose marques empaquetées, clés vivantes isolées et pic de pile. Aucun ne mesure le RSS global.

L’ouverture valide linéairement les index complets. Audits/GC/compaction en flux réduisent les inventaires empaquetés, mais clés isolées, métadonnées, piles, codecs et caches coûtent toujours. Checkout reconstruit les fichiers changés complets et utilise espace privé/validation complète. Watch cible des sous-arbres entre audits complets. Rapports synthétiques et scripts de reproduction restent des données développeur hors manuel ; leurs durées ne promettent pas un débit universel.

## Capture incrémentale des fichiers de travail {#incremental-capture}

Agents natifs résidents et captures C/Go réutilisent les manifestes ingérés par
leur session si parent publié, contexte et matérialisation correspondent, avec
identité et empreinte de changement fiables inchangées. Ils inventorient toute
la sélection : renommages, suppressions et exclusions. Fermer puis rouvrir le
Store conserve la preuve de session ; index de statut et notifications ne la
fournissent jamais. La fermeture complète reste vérifiée, parents historiques et
manifestes réutilisés compris ; corruption ou absence refusent la publication.

Fichiers récents (garde de deux secondes), écrivains Windows actifs et systèmes
distants/non pris en charge utilisent l’ingestion ordinaire. Empreintes sur
NTFS/ReFS local, ext4/XFS/Btrfs/tmpfs Linux et APFS macOS. Redémarrage,
invalidation explicite, échec natif ou changement de parent/sélection effacent la
réutilisation. `native commit` ponctuel continue de lire toutes les sources.

Le JSON C ajoute `capture: {version, ingested_files, reused_files,
source_bytes_read, source_bytes_reused}`. Go expose `CaptureReportContext` ;
`CaptureContext` retourne toujours l’ID. C++ expose `WorkspaceCapture` dans
`<syrnck/workspace.hpp>`. Les compteurs couvrent les sources, sans métadonnées,
vérification de fermeture ni cache système.

Construire `syrnck_capture_benchmark`, puis lancer :

```text
python scripts/benchmark_capture.py --exe CHEMIN_BENCHMARK --mib 128 --files 64 --iterations 5 --output capture.json
```

Ajouter `--encrypted` pour le chiffrement au repos. Ce banc synthétique alterne
captures complètes/de session, inclut ouverture et publication complète, sépare
les E/S du Store et vérifie chaque résultat par lecture complète. Il ne garantit
aucun débit sur un projet Unreal. Vérification historique et métadonnées plates
restent des limites de passage à l’échelle.

Une capture réussie initialise l’observateur avec son inventaire sélectionné
complet ; le tick natif suivant peut cibler les chemins changés. Seuls les lots
antérieurs sont consommés ; les notifications pendant/après capture restent en
attente. Un audit périodique impose toujours une lecture complète. Ce statut
reste une observation à l’instant de capture, pas un instantané atomique des
écritures concurrentes.
