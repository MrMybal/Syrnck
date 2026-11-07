# Mesures et stratégie de performance

## Reference tables and JSON depth validation — 4 November 2026

Reference tables use a tree-backed parser and direct sorted serialization. A
separate SAX depth guard also removes the DOM callback parser's repeated parent
scans. On Windows x64 Release, three alternating measurements of the complete
CLI `refs` command gave medians of **8.941 s before** and **0.296 s after** for
32,768 references. At 8,192 references, the medians were 0.599 s and 0.064 s.
Output was byte-identical and full-file audits passed after the runs.

See [methodology, validation and remaining limits](../../../Documentation/Documentation.html#/en/reference-tables), the
[raw report](benchmarks/reference-tables-windows-x64-2026-11-04.json), and
`scripts/benchmark_refs.py`. These synthetic reference-listing measurements do
not establish asset throughput, network performance or memory savings.

## Streaming compaction plans — 4 November 2026

Packing and compaction now merge packed indexes by pages, plan output with counts,
byte sizes and sequence digests, and read back new objects without full packed-ID
lists. Loose-only IDs still require sorting. Publication and envelope verification
remain mandatory.

The same measurement harness was linked to engine commit `5bca803` and the updated
engine. Over 131,137 valid objects, three alternating Windows x64 Release runs
measured median peak working sets of **50.72 MiB before** and **16.30 MiB after**.
Complete compaction medians were **20.764 s before** and **22.079 s after**; these
variable timings establish no speed improvement. Every run produced the same
counters and passed a separate full-file audit after publication.

See [methodology, reproduction and remaining limits](../../../Documentation/Documentation.html#/en/compaction) and the
[six-run report](benchmarks/streaming-compaction-windows-x64-2026-11-04.json).

## Streaming integrity audit — 4 November 2026

`fsck` now enumerates objects by kind without materializing a complete inventory,
using one mark bit per packed entry. It still verifies unreachable metadata,
complete file hashes and both physical copies of loose/packed duplicates.

For 131,137 valid objects (131,072 distinct 64-byte blocks, 64 manifests and one
revision), three alternating Windows x64 Release runs measured a median peak
working set of **11.64 MiB**, versus **85.27 MiB** for a comparison implementation
retaining the previous inventories. Median times were **8.312 s** and **8.537 s**.
Both paths read and decoded all 131,137 objects, verified every file hash and left
the store byte-identical. The comparator uses current codecs/indexes in the same
binary, rather than a historical executable; OS caches are uncontrolled.

See [audit guarantees, methodology and limits](../../../Documentation/Documentation.html#/en/audit), the
[complete report](benchmarks/streaming-audit-windows-x64-2026-11-04.json), and
`scripts/benchmark_audit.py` for reproduction. Loose-only IDs, dependency stacks
and metadata still consume memory; this is not a global constant-memory claim
or a benchmark of Unreal assets.

## GC en flux et marquage compact — 4 novembre 2026

Le GC ne construit plus de collections complètes pour l'inventaire des objets, les objets morts et les survivants de chaque pack. Il parcourt les index par pages, marque chaque objet empaqueté avec un bit et recopie les survivants en flux. Les hashes complets des fichiers conservés restent vérifiés avant toute suppression. Voir les [garanties et limites mémoire](../../../Documentation/Documentation.html#/en/packs/memoire-du-gc).

Le banc utilise **131 072 blocs distincts de 64 octets**, 64 manifestes de fichiers et une révision, soit **131 137 objets valides** dans un pack. La révision conserve 32 fichiers ; les autres manifestes sont inaccessibles. Le plan contient 65 569 objets vivants et 65 568 objets morts. Les petites tailles sont choisies pour observer le coût des identifiants ; elles ne représentent pas le découpage CDC par défaut ni des assets Unreal. Les enveloppes sont réellement compressées, les métadonnées canoniques et les hashes des fichiers calculés à partir de leurs blocs.

Une implémentation de comparaison reproduit les collections de l'ancien GC (`set` de clés vivantes, vecteur d'objets morts et listes de clés par pack), avec les codecs et index actuels dans le même binaire. Ce n'est pas un ancien exécutable produit. Elle utilise un lecteur auxiliaire sans cache de pages, avec une session réutilisant ses handles, en plus du Store commun doté de 4 Mio de cache d'index. Les différences de parcours IO et ce lecteur auxiliaire empêchent d'interpréter les temps comme une comparaison exacte entre deux versions du produit. L'objectif principal est la mémoire des inventaires.

Trois répétitions alternent les modes dans des processus neufs, sous Windows x64 Release MSVC 19.40. La fixture est préparée dans un processus séparé. Le chiffrement et le cache d'objets décodés sont désactivés ; le Store conserve son budget de pages de 4 Mio. Les compteurs de mémoire ont les mêmes [définitions Windows que le banc d'index ci-dessous](#index-paginés-et-mémoire--4-novembre-2026), et comprennent le runtime, les allocations temporaires et les caches du processus. Les caches OS et la charge environnante ne sont pas contrôlés.

Médianes des simulations de GC :

| Chemin | Pic résident du processus | Temps de simulation |
| --- | ---: | ---: |
| Inventaires complets, comparateur | 55,31 Mio | 5,866 s |
| GC en flux | 12,12 Mio | 4,448 s |

Le pic résident est environ **4,6 fois plus faible** dans ce cas. Le bitmap utile occupe **16 393 octets**, avec zéro clé vivante isolée et au plus 32 clés en attente dans la pile ; ces trois compteurs ne représentent pas toute la RAM. Les six simulations obtiennent exactement les mêmes compteurs de rétention, d'octets récupérables et de réécriture. Les deux chemins relisent et décodent les mêmes 65 569 objets vivants et vérifient tous les fichiers conservés.

Une septième exécution effectue réellement la suppression : un pack de remplacement de 7 649 532 octets est publié, puis les anciennes copies sont retirées. Cette exécution unique prend 5,859 s, avec un pic résident de 15,18 Mio mesuré avant l'audit complémentaire. L'audit `fsck` après suppression revalide les 65 569 objets conservés et leurs fichiers ; une nouvelle simulation ne trouve aucun objet mort. Les compteurs IO de cette ligne du rapport incluent les audits complémentaires, alors que le temps et la mémoire sont relevés avant eux. Ce résultat ne constitue pas une médiane de suppression.

Le [rapport complet](benchmarks/streaming-gc-windows-x64-2026-11-04.json) conserve les sept résultats, plans, compteurs, mémoire et SHA-256 du binaire. Reproduction :

```text
python scripts/benchmark_gc.py --exe build/Release/syrnck_gc_bench.exe --output rapport-gc.json
```

Sur Unix, utiliser `build/syrnck_gc_bench` ; les compteurs mémoire y retournent actuellement `null`. Les tests fonctionnels couvrent séparément le chiffrement et le stockage mixte. Les objets vivants isolés, la pile et les métadonnées restent en RAM ; l'API publique `closure()` conserve encore son inventaire complet. La compaction utilise maintenant les [plans en flux](../../../Documentation/Documentation.html#/en/compaction). L'audit `fsck` a depuis reçu le parcours en flux décrit ci-dessus ; les résultats du GC restent ceux du binaire historique indiqué dans leur rapport. Aucune extrapolation à un projet réel, à un GC distribué ou à une RAM globalement constante n'est faite.

## Index paginés et mémoire — 4 novembre 2026

Le lecteur de packs ne prépare plus une `std::map` contenant chaque ID et sa position. Il vérifie l'index en flux puis garde un répertoire de pages, un filtre de clés absentes et un cache de pages configurable. L'index binaire v2/v3 reste identique. Les nouvelles écritures d'index sont également effectuées en flux. Voir les [garanties du lecteur et ses budgets](../../../Documentation/Documentation.html#/en/indexes).

Le banc compare ce lecteur à une **implémentation de comparaison du modèle antérieur**, avec lecture de tout l'index puis construction d'une `std::map<ObjectKey, Entry>`, dans le même binaire Release MSVC 19.40. Il ne lance pas un ancien binaire produit. Le fichier synthétique contient **1 048 576 entrées** et occupe **45 Mio + 16 octets**. Ses IDs sont artificiels, triés et séparés par des clés absentes ; ses enveloppes opaques ne sont pas des objets Zstd. Ce banc vérifie des recherches d'adresses, sans mesurer codecs, authentification d'objet, reconstruction, réseau ou scan d'un projet.

Chaque exécution vérifie l'index complet avant deux passages de 20 000 recherches identiques, dont la moitié présentes et la moitié absentes. Les résultats sont contrôlés un par un. Deux motifs : clés dispersées déterministes, puis clés voisines. Chaque passage du lecteur paginé ouvre une session bornée réutilisant les fichiers d'index, comme les parcours de reconstruction, fermeture et ingestion du moteur. Les handles sont fermés entre passages ; les pages du cache sont conservées. Des appels isolés sans session peuvent coûter plus cher à froid. Trois répétitions alternent l'ordre des modèles, avec des budgets de pages de 0, 4 et 64 Mio. Le premier passage commence avec un cache applicatif vide ; cela ne signifie pas un disque froid.

La fixture est créée dans un processus séparé, hors chronométrage et mesures de mémoire du lecteur. Les mesures Windows proviennent de [`GetProcessMemoryInfo`](https://learn.microsoft.com/en-us/windows/win32/api/psapi/nf-psapi-getprocessmemoryinfo) : le working set décrit la mémoire résidente du processus, son pic inclut l'ouverture, et la mémoire privée décrit l'engagement privé, pas le RSS. Voir les [définitions Microsoft](https://learn.microsoft.com/en-us/windows/win32/api/psapi/ns-psapi-process_memory_counters_ex). Le processus inclut runtime, allocations, répertoires, cache et buffers ; ces compteurs sont distincts des seuls octets utiles retournés par `Store::io_stats()`. Ils ne mesurent pas le cache de fichiers global du système.

Médianes sur les recherches dispersées, mémoire résidente après les deux passages :

| Modèle | Budget de pages | Résident du processus | Ouverture | 1er passage | 2e passage |
| --- | ---: | ---: | ---: | ---: | ---: |
| Table en mémoire | — | 212,86 Mio | 0,570 s | 471 kreq/s | 472 kreq/s |
| Paginé | 0 Mio | 5,73 Mio | 0,181 s | 98,5 kreq/s | 96,8 kreq/s |
| Paginé | 4 Mio | 10,24 Mio | 0,183 s | 89,2 kreq/s | 93,1 kreq/s |
| Paginé | 64 Mio | 52,79 Mio | 0,176 s | 171,8 kreq/s | 808,0 kreq/s |

Avec 4 Mio, ce motif dispersé dépasse largement le cache et provoque 10 087 lectures de pages à chaque passage : la faible mémoire se paie ici par un débit inférieur à celui de la table en RAM. Avec 64 Mio, tout l'index de 45 Mio finit en cache ; le second passage ne relit aucune page et atteint 808 kreq/s, contre 472 kreq/s pour la table de comparaison, avec environ quatre fois moins de mémoire résidente. Le pic résident de la table atteint 257,87 Mio pendant son chargement, contre 10,24 Mio pour le lecteur avec cache de 4 Mio.

Sur les clés voisines, le cache de 4 Mio ne lit que 40 pages au premier passage, puis zéro. Il atteint 1,52 et 1,57 million de recherches/s, contre 4,87 et 5,17 millions pour la table en RAM. Sa mémoire résidente est d'environ 6,11 Mio. Ces différences montrent un compromis de localité, de CPU et de mémoire ; il n'y a pas de gain de débit universel.

Le [rapport des 24 exécutions](benchmarks/paged-indexes-windows-x64-2026-11-04.json) conserve les temps, allocations utiles, compteurs de pages, mémoire après ouverture/recherche et SHA-256 du binaire. Les caches du système et la charge ambiante ne sont pas contrôlés. Les recherches ne sont pas un débit de fichiers : leur compromis RAM/IO dépend de la localité et du budget choisi. Les mesures mémoire natives sont actuellement produites sous Windows ; le banc retourne `null` pour ces compteurs sur les autres OS.

```text
python scripts/benchmark_indexes.py --exe build/Release/syrnck_index_bench.exe --output rapport-index.json
```

La réduction de RAM concerne le lecteur d'index. L'ouverture reste un parcours complet des index, et les inventaires, fermetures et plans de maintenance peuvent encore utiliser une mémoire proportionnelle au nombre d'objets. Les plafonds de catalogue ne sont pas relevés par cette optimisation.

## Maintenance incrémentale de plusieurs packs — 4 novembre 2026

La maintenance ordinaire ajoute les objets nouveaux dans des packs séparés, en conservant les packs existants. `pack --compact` réécrit explicitement l'ensemble des objets. Les deux chemins vérifient tous les objets, les dépendances et les hashes des fichiers avant publication. Le GC, de son côté, préserve les packs entièrement vivants, retire ceux entièrement morts et réécrit seulement les packs partiellement morts.

Cette mesure antérieure au lecteur paginé utilise un fichier pseudo-aléatoire de **32 Mio** découpé en blocs fixes de **64 Kio**, empaqueté une première fois. Une seconde version ajoute exactement un bloc de 64 Kio ; les deux versions restent épinglées. Le stock final contient 515 objets, dont seulement deux nouveaux : le bloc ajouté et le nouveau manifeste. Deux stores équivalents sont préparés séparément pour comparer `pack` et `pack --compact`, avec cache décodé désactivé et ordre alterné. Trois répétitions par mode et par chiffrement, Windows x64 Release MSVC 19.40.

Le script compare les IDs, vérifie chaque reconstruction et l'audit final, puis rapproche les compteurs d'écriture des tailles des nouveaux fichiers effectivement publiés. En mode incrémental, il contrôle aussi que les anciens packs et index conservent leurs octets et dates d'écriture. Les volumes ci-dessous portent sur les fichiers pack complets, hors index, configuration, copies temporaires internes et métriques physiques du périphérique.

Médianes de maintenance et volumes des nouveaux fichiers de packs :

| Chiffré | Incrémental | Compaction complète | Rapport de temps | Octets de pack incrémental / complet |
| --- | ---: | ---: | ---: | ---: |
| Non | 0,307 s | 0,524 s | 1,71× | 83 527 / 33 663 575 |
| Oui | 0,543 s | 0,994 s | 1,83× | 83 607 / 33 684 175 |

Le mode incrémental publie deux objets, contre 515 pour la compaction complète, soit environ 400 fois moins de nouveaux octets de pack dans ce cas. Les nouveaux index occupent respectivement 106 et 23 191 octets. Le volume total conservé reste comparable, avec deux packs actifs au lieu d'un : les deux versions logiques restent disponibles dans les deux modes.

Le [rapport des 12 exécutions](benchmarks/incremental-packs-windows-x64-2026-11-04.json) conserve les temps, volumes, IDs et SHA-256 du binaire. Il ne mesure pas l'ingestion, la préparation initiale, la reconstruction de contrôle ou le réseau dans le chronométrage de maintenance. Le démarrage CLI, la validation complète, la construction/relecture des nouveaux packs, les flushs, la publication et le nettoyage sont inclus. Les caches OS et la charge ambiante ne sont pas contrôlés. Les résultats décrivent une petite modification synthétique, sans extrapolation à des projets réels ou à des millions d'objets.

```text
python scripts/benchmark_incremental_packs.py --exe build/Release/syrnck.exe --output rapport-maintenance.json
```

La validation continue à relire le stock : les écritures évitées ne suppriment pas ce coût. Cette mesure employait encore la table d'index en mémoire, remplacée depuis par le lecteur paginé. Les tests du GC vérifient séparément que les packs non concernés restent inchangés et que la suppression de packs entièrement morts ne crée aucun pack de remplacement. Voir [réglages, versions et limites](../../../Documentation/Documentation.html#/en/packs).

## Packs et sessions de lecture — 4 novembre 2026

Cette mesure initiale, réalisée avant le catalogue de packs multiples, porte sur un seul pack. Le stockage optionnel réduit le nombre de fichiers et partage un handle de lecture pendant chaque reconstruction ou audit. Le contenu des enveloppes et les IDs restent identiques. Le cache décodé est désactivé dans cette comparaison pour mesurer le chemin de lecture complet.

Mesure locale Windows x64, Release MSVC 19.40 : même fichier synthétique pseudo-aléatoire de **8 Mio**, blocs fixes de **4 Kio**, deux stores préparés séparément avec le même binaire. Le fichier produit 2 048 blocs distincts et un manifeste, soit **2 049 fichiers isolés contre 2 fichiers de pack/index**. Cinq reconstructions par disposition et mode de chiffrement, avec ordre alterné ; une reconstruction non chronométrée prépare les caches OS de chaque store avant les mesures.

Médianes de reconstruction, en Mio/s logiques :

| Chiffré au repos | Fichiers isolés | Pack indexé | Rapport |
| --- | ---: | ---: | ---: |
| Non | 19,85 | 35,95 | 1,81× |
| Oui | 18,17 | 31,15 | 1,71× |

Les chronométrages incluent le démarrage du CLI, l'ouverture du store, le chargement de l'index lorsqu'il existe, la validation/reconstruction et le flush du nouveau fichier de sortie. La préparation des stores, le regroupement et la comparaison SHA-256 des sorties sont hors de ces chronométrages. Le script vérifie les IDs, chaque reconstruction et le nombre de fichiers ; il conserve aussi un temps de regroupement par store chiffré/non chiffré, distinct des cinq reconstructions.

Ce cas favorise l'observation du coût des nombreux petits fichiers. Il ne représente pas le découpage CDC par défaut à moyenne cible de 1 Mio, un projet Unreal réel, un accès disque froid, le réseau ou une compaction de très grande taille. Les caches système et l'activité de la machine restent non contrôlés. Les gains ne sont pas extrapolés à ces autres usages. Le binaire mesuré chargeait encore tout l'index en RAM et réécrivait son pack actif ; ces coûts ont été traités ensuite par le catalogue incrémental et le lecteur paginé.

Le [rapport des 20 reconstructions](benchmarks/packs-windows-x64-2026-11-04.json) contient les résultats individuels, IDs, fichiers physiques, temps de regroupement et SHA-256 du binaire. Reproduction :

```text
python scripts/benchmark_packs.py --exe build/Release/syrnck.exe --repetitions 5 --output rapport-packs.json
```

Sur Unix, utiliser `build/syrnck`. Les paramètres `--mib` et `--block-kib` changent le corpus ; les données doivent être réingérées et les résultats interprétés avec leur nombre de blocs. Voir [activation et limites des packs](../../../Documentation/Documentation.html#/en/packs).

## Lectures vérifiées et parcours fusionné — 4 novembre 2026

Le moteur fusionne la traversée des dépendances d'un manifeste et la vérification de son hash de fichier. Pour un fichier à blocs distincts, une validation froide lit maintenant un manifeste et chacun de ses blocs une fois. Les blocs partagés entre fichiers ou répétés dans un fichier restent nécessaires au calcul de chaque hash complet. `fsck` réunit aussi les objets énumérés et les références dans une seule traversée.

Un cache local au handle réutilise les données décodées et les preuves d'une validation antérieure **après relecture et BLAKE3 de l'enveloppe actuelle**. Il n'évite pas les octets d'IO de ces accès. Les valeurs par défaut sont 64 Mio de données décodées et 8 192 empreintes, avec éviction LRU indépendante. Aucun index de validation n'est sauvegardé sur disque. `fsck` et `gc` contournent ce cache pour leurs validations.

Mesure locale avec le même binaire Release MSVC 19.40, Windows x64 : fichiers synthétiques de **32 Mio**, blocs fixes de 1 Mio, cache désactivé ou limité à 64 Mio, cinq répétitions en alternant l'ordre des deux modes. Le store est neuf à chaque exécution. Une publication de référence et une lecture de métadonnées préparent le cache dans le même handle avant les chronométrages. Le fichier et ses métadonnées tiennent entièrement dans le budget ; cette mesure décrit donc un cas favorable de **cache chaud**. Elle ne représente pas deux commandes CLI indépendantes ni un jeu de travail dépassant le cache.

Médianes sur les données pseudo-aléatoires, en Mio/s logiques de fichier :

| Opération | Chiffré au repos | Cache désactivé | Cache chaud | Rapport |
| --- | --- | ---: | ---: | ---: |
| Présence de tous les blocs | Non | 487,0 | 714,0 | 1,47× |
| Réingestion inchangée | Non | 341,6 | 488,9 | 1,43× |
| Publication de référence | Non | 428,5 | 509,5 | 1,19× |
| Reconstruction | Non | 298,6 | 361,5 | 1,21× |
| Présence de tous les blocs | Oui | 183,6 | 706,6 | 3,85× |
| Réingestion inchangée | Oui | 161,8 | 490,7 | 3,03× |
| Publication de référence | Oui | 180,5 | 536,1 | 2,97× |
| Reconstruction | Oui | 146,4 | 333,0 | 2,28× |

Les contrôles de présence lisent toujours 32 enveloppes, et les quatre autres opérations 33 dans ce scénario. Cache chaud, les opérations hors audit passent de 32 ou 33 décodages complets à zéro. `fsck` conserve ses 33 lectures et 33 décodages dans les deux configurations ; ses variations de temps ne représentent pas une accélération par cache. Le second motif mesuré répète 32 fois un bloc de 1 Mio : les compteurs et débits correspondants figurent dans le rapport complet.

Les IDs et reconstructions sont comparés entre les modes, les budgets contrôlés et l'absence de hits de cache dans `fsck` vérifiée. Les tests couvrent aussi les évictions, suppressions, réouvertures, changements d'enveloppe valides, corruption à taille/mtime identiques et échec du GC avant suppression. Ces preuves fonctionnelles sont distinctes des mesures de vitesse.

Les timings incluent les copies en mémoire du banc et les écritures de référence quand l'opération en effectue. La génération des données, l'ingestion initiale et la préparation du cache sont hors chronométrage. Les caches du système et la charge de la machine ne sont pas contrôlés ; aucun résultat réseau, corpus réel ou gain après réouverture du store n'est revendiqué. Une première lecture ajoute le coût de l'empreinte d'enveloppe et éventuellement une copie pour le cache ; un accès sans réutilisation peut en subir le coût sans bénéficier de hits.

Le [rapport des 40 exécutions](benchmarks/verified-reads-windows-x64-2026-11-04.json) conserve les compteurs IO, tailles de cache, IDs, résultats et SHA-256 du binaire. Reproduction :

```text
python scripts/benchmark_reads.py --exe build/Release/syrnck_read_bench.exe --output rapport-lectures.json
```

Sur Unix, utiliser `build/syrnck_read_bench`. Les mesures Linux/macOS et les index persistants restent à réaliser.

## Pipeline C++20 borné — 3 novembre 2026

Le moteur propose maintenant une ingestion parallèle optionnelle : compression et chiffrement des blocs nouveaux dans un pool, écritures coordonnées dans l'ordre. Le découpage, les hashes et la validation des objets présents restent dans le coordinateur. Le mode séquentiel reste le défaut et les autres commandes que `put` ne propagent pas encore ces réglages.

Comparaison locale avec le **même binaire Release MSVC 19.40**, Windows x64 : 64 Mio par fichier, insertion de 34 octets à l'offset 1 Mio, 1 ou 4 workers, budget de 32 Mio, cinq répétitions avec ordre des deux modes alterné. Deux motifs sont générés : pseudo-aléatoire, puis compressible avec des octets variables régulièrement espacés. Chaque exécution emploie un store neuf et compare la reconstruction octet par octet. Les IDs des deux versions sont aussi comparés entre tous les modes, dont les stores chiffrés avec une clé publique réservée au benchmark.

Médianes d'ingestion **initiale**, en Mio/s :

| Données | Politique | Chiffré au repos | 1 worker | 4 workers | Rapport |
| --- | --- | --- | ---: | ---: | ---: |
| Pseudo-aléatoires | FastCDC | Non | 138,6 | 151,6 | 1,09× |
| Pseudo-aléatoires | Fixe 1 Mio | Non | 146,9 | 172,1 | 1,17× |
| Pseudo-aléatoires | FastCDC | Oui | 102,8 | 151,5 | 1,47× |
| Pseudo-aléatoires | Fixe 1 Mio | Oui | 110,5 | 169,2 | 1,53× |
| Compressibles | FastCDC | Non | 353,9 | 455,6 | 1,29× |
| Compressibles | Fixe 1 Mio | Non | 215,0 | 259,6 | 1,21× |
| Compressibles | FastCDC | Oui | 347,7 | 463,3 | 1,33× |
| Compressibles | Fixe 1 Mio | Oui | 214,3 | 257,3 | 1,20× |

Sur les données pseudo-aléatoires modifiées, FastCDC ne stocke toujours que 1 885 318 octets nouveaux et réutilise 65 223 580 octets. Le débit médian de cette seconde ingestion varie peu et légèrement à la baisse avec 4 workers : 368,9 → 364,7 Mio/s en clair et 194,7 → 191,8 Mio/s chiffré. La vérification des objets réutilisés et le lancement du pool peuvent dominer quand il reste peu à encoder. Le mode parallèle n'est donc pas activé automatiquement.

Ces chiffres concernent des corpus synthétiques sur une seule machine, avec caches système et activité environnante non contrôlés. Ils ne mesurent ni réseau, ni scan de projet, ni validation/publication d'une référence. La reconstruction utilise le même chemin dans les deux modes ; ses variations entre sessions ne constituent pas un gain dû au pipeline. Les temps incluent l'écriture des objets et les flushs existants. Les fichiers d'entrée et de sortie en mémoire du benchmark s'ajoutent à sa RAM ; le budget configuré ne représente pas la mémoire totale du processus.

Les rapports complets conservent les 40 résultats par motif, leurs identifiants, les maxima de tâches/octet en attente et le SHA-256 du binaire : [pseudo-aléatoire](benchmarks/pipeline-random-windows-x64-2026-11-03.json), [compressible](benchmarks/pipeline-compressible-windows-x64-2026-11-03.json).

Reproduction après construction :

```text
python scripts/benchmark_pipeline.py --exe build/Release/syrnck_bench.exe --pattern random --output rapport-random.json
python scripts/benchmark_pipeline.py --exe build/Release/syrnck_bench.exe --pattern compressible --output rapport-compressible.json
```

Sur Unix, remplacer l'exécutable par `build/syrnck_bench`. Le programme accepte directement `MiB workers encrypted bufferMiB pattern`, avec `encrypted` valant 0 ou 1. Le script vérifie les reconstructions, les IDs et les bornes avant d'écrire un rapport. Les résultats Linux/macOS ne sont pas encore mesurés localement.

## Mesure C++20 antérieure au pipeline — 3 novembre 2026

Après la construction CMake, exécuter `build/Release/syrnck_bench.exe 64` sous Windows, ou `build/syrnck_bench 64` avec le preset Unix.

Mesure locale unique : Windows x64, MSVC 19.40.33821, CMake Release `/O2`. Même flux pseudo-aléatoire de 64 Mio et insertion de 34 octets à l'offset 1 Mio. Le store est non chiffré et le réseau n'est pas mesuré. La création des flux d'entrée en mémoire est hors chronométrage.

| Politique | Octets nouveaux | Octets réutilisés | Ingestion initiale | Reconstruction |
| --- | ---: | ---: | ---: | ---: |
| FastCDC 2020 | 1 885 318 | 65 223 580 | 238,0 Mio/s | 564,2 Mio/s |
| Blocs fixes de 1 Mio | 66 060 322 | 1 048 576 | 276,5 Mio/s | 428,7 Mio/s |

Les frontières, nombres de blocs et octets réutilisés sont identiques au prototype Rust ci-dessous. Chaque reconstruction est comparée octet par octet. Les mesures des deux implémentations proviennent de sessions et de constructions différentes : elles ne constituent pas une comparaison contrôlée des langages. Les accès disque, caches et allocations affectent les temps observés.

Les résultats bruts sont dans [benchmarks/cpp20-windows-x64-2026-11-03.json](benchmarks/cpp20-windows-x64-2026-11-03.json). Les performances Unreal, le réseau et le chiffrement restent à mesurer.

## Mesure historique du prototype Rust — 3 novembre 2026

Commande historique du prototype Rust, conservée pour la provenance des chiffres :

```text
cargo run --release --locked -p syrnck-core --example chunk_bench -- 64
```

Environnement observé : Windows x64, Rust 1.99.1, MSVC, profil release avec LTO thin. Les fichiers temporaires utilisent le stockage local. Cette mesure unique n'isole pas le cache du système, les effets thermiques ou le débit physique du disque.

Le corpus est un flux pseudo-aléatoire déterministe de **64 Mio**, présent en mémoire. Une insertion de 34 octets est effectuée à l'offset 1 Mio. Chaque politique emploie son propre store vierge, non chiffré. La première ingestion écrit les blocs ; la seconde réutilise ce qui peut l'être. Les débits incluent le stockage et la compression, mais pas la publication d'une référence, le réseau, le scan de répertoire ou le GC.

| Politique | Octets nouveaux après insertion | Octets réutilisés | Réutilisation | Ingestion initiale | Reconstruction |
| --- | ---: | ---: | ---: | ---: | ---: |
| FastCDC 2020, 256 Kio / 1 Mio / 4 Mio | 1 885 318 | 65 223 580 | 97,19 % | 254,0 Mio/s | 339,4 Mio/s |
| Blocs fixes de 1 Mio | 66 060 322 | 1 048 576 | 1,56 % | 273,7 Mio/s | 347,6 Mio/s |

Le flux modifié contient 67 108 898 octets. Chaque reconstruction est comparée octet par octet au flux attendu. Les résultats bruts sont dans [benchmarks/windows-x64-2026-11-03.json](benchmarks/windows-x64-2026-11-03.json).

L'intérêt observé de CDC est la récupération de frontières communes après le décalage des octets. Cette mesure ne prouve pas un gain équivalent sur les `.uasset`, fichiers compressés, réécritures complètes ou ressources déjà chiffrées. Elle ne mesure pas le réseau ni le chiffrement. Le nombre d'octets nouveaux décrit les données brutes des blocs et exclut le manifeste, l'enveloppe et le protocole.

## Coûts actuels

- Lecture des données en flux, avec une mémoire liée au bloc maximum et aux codecs ; les listes de blocs et de fichiers restent en mémoire.
- Pipeline optionnel : au maximum le budget d'octets bruts configuré et `2 × workers` tâches. Les buffers encodés, contextes par worker, tampon de lecture et métadonnées s'ajoutent à ce budget. La limite sérialisée du manifeste v1 est vérifiée pendant l'ingestion pour éviter une croissance sans limite de la liste de blocs.
- Écritures nouvelles : un fichier par objet et un `fsync` par publication. La maintenance `pack` regroupe ces objets dans deux fichiers actifs ; les nouvelles écritures restent isolées jusqu'au regroupement suivant. Le verrou reste exclusif pour tout le store.
- Packs : ajout incrémental, compaction explicite et GC sélectif. Index paginés avec répertoire/filtre en RAM et 4 Mio de pages par défaut ; les limites restent 1 048 576 objets par pack, 1 024 packs et 4 194 304 objets au total. Une session garde au plus 8 handles de données et 8 d'index avec LRU. La fusion automatique reste à développer.
- Les blocs réutilisés sont relus. Un hit du cache vérifié évite les codecs après contrôle de l'enveloppe ; une lecture froide réalise encore toute la validation.
- La publication d'une référence revalide sa fermeture, y compris les hashes complets des fichiers et les parents, dans le parcours fusionné. Son coût peut toujours croître avec l'historique. L'indexation persistante et la validation incrémentale restent à développer.
- Le cache décodé, les structures d'empreintes et leurs LRU s'ajoutent aux autres budgets mémoire. Le cache est borné mais pas la somme de toutes les allocations du processus par un unique réglage.
- Serveur sérialisé et agent séquentiel ; aucune revendication de passage à l'échelle multi-utilisateur à ce stade.
- Formats JSON et manifestes plats bornés à 16 Mio : le coût des métadonnées dépend du nombre de fichiers/blocs. Les limites se calculent sur la représentation sérialisée, pas seulement sur la taille des données.

## Prochain banc de mesure

Utiliser un corpus autorisé de vrais projets : code, cartes Unreal, World Partition, textures, audio, plugins et données déjà compressées. Inclure fichiers inchangés, modifications au milieu, insertions, suppressions, renommages, retours à une ancienne version et fichiers dépassant la mémoire disponible.

Mesurer séparément scan froid/chaud, CDC, hash, compression, chiffrement, validation, publication, fsck, GC et transfert. Rapporter temps p50/p95, RAM maximale, CPU, lectures/écritures, taille de métadonnées, taille compressée et octets réseau effectifs. Tester latence élevée, bande passante réduite, reprise et plusieurs clients.

Les optimisations suivantes sont l'index transactionnel d'objets vérifiés, les arbres de métadonnées, le tri et le marquage bornés des objets isolés, la fusion automatique des packs, la propagation des réglages du pipeline aux autres commandes, les transferts groupés et le suivi incrémental du disque. Les tests de reconstruction et de rétention doivent rester invariants pendant ces changements.
