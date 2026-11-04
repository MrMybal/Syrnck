# Lecture paginée des index de packs

Le moteur consulte les index `SYRIDX01` directement sur disque. Il conserve en mémoire un répertoire de pages, un filtre de clés absentes et un cache de pages borné, au lieu d'une table complète contenant chaque ID et son emplacement.

Cette évolution ne change aucun octet du format physique v2/v3, des objets, des enveloppes ou du protocole réseau. Elle ne crée aucun fichier d'index supplémentaire et ne convertit pas les stores à l'ouverture.

## Validation et recherche

Chaque ouverture ou audit parcourt **tout l'index** avec un tampon de 256 enregistrements, soit **11 520 octets**. Le lecteur contrôle l'en-tête, le nombre d'entrées, les types, le tri strict, la continuité des plages et le hash BLAKE3 complet annoncé par la configuration. Il construit pendant ce parcours :

- Pour chaque page : sa première et dernière clé et son empreinte BLAKE3.
- Pour chaque pack : un filtre de présence à 10 bits par entrée environ, avec sept sondes.

Le filtre permet de rejeter des clés absentes sans lire une page. Une réponse positive reste à confirmer dans l'index ; le filtre ne valide pas un objet. Les bornes dirigent une recherche binaire vers la page candidate. À l'ouverture du fichier d'index, le moteur vérifie le fichier et sa taille. Chaque page relue est comparée à son empreinte du parcours validé avant utilisation de ses IDs et emplacements. Une page changée, tronquée ou absente produit une erreur.

Les index des packs sont triés séparément. Quand leurs intervalles de clés se recouvrent, une fusion de leurs parcours vérifie qu'aucune clé n'appartient à deux packs actifs. Ce contrôle garde une page par pack concerné et une file de têtes, sans reconstruire une table de tous les objets. Avec 1 024 packs, les seules pages de ce contrôle temporaire peuvent atteindre 11,25 Mio. Les intervalles entièrement disjoints dispensent de cette fusion.

## Cache et snapshot

Les pages déjà vérifiées et la dernière adresse trouvée forment un **snapshot du catalogue ouvert**, comme l'ancienne table entièrement en RAM. Une modification externe d'un index pendant que le store détient son verrou peut donc rester invisible sur une entrée chaude. Les pages relues après éviction doivent toujours correspondre à leurs empreintes validées. `fsck` et `gc` rechargent les index depuis leur début et vérifient leurs hashes complets avant leur travail ; ils n'acceptent pas une ancienne page comme preuve de l'état actuel du disque.

Cette règle porte sur les adresses dans l'index. Les enveloppes d'objets restent relues et contrôlées avant toute réutilisation du cache décodé. Le contenu retourné doit satisfaire les mêmes règles d'ID, format et authentification que précédemment. Le filtre, le répertoire et les pages ne sont pas des racines de rétention.

## Réglages et compteurs

Le cache de pages d'index vaut **4 Mio par défaut**, indépendamment du cache décodé de 64 Mio. L'option globale accepte 0 à 1 024 Mio ; zéro désactive le cache de pages et de la dernière adresse, tout en conservant le répertoire et le filtre nécessaires au lecteur :

```text
syrnck --index-cache-mib 4 get <manifeste> nouvelle-copie.bin
syrnck --cache-mib 0 --index-cache-mib 0 fsck
```

En C++, utiliser `StoreOptions::pack_index_cache_bytes` au quatrième argument du constructeur `Store`. Le budget est transmis aux lecteurs temporaires créés pendant la maintenance. Il porte sur les données de pages de chaque lecteur ; les snapshots temporaires, structures de cache et buffers de travail s'ajoutent à ce budget. La LRU est partagée par les index d'un catalogue, sans multiplier le budget par le nombre de packs. Une page trop grande pour le budget est lue dans un tampon temporaire puis libérée/remplacée, sans admission dans le cache.

`io_stats()` expose quatre instantanés d'allocations utiles au réglage : `index_directory_bytes`, `index_filter_bytes`, `index_cached_bytes`, `index_cached_pages`. Ils comptent les capacités des répertoires/filtres et les données de pages admises, sans l'overhead des conteneurs/allocateurs, les chemins, métadonnées JSON, buffers temporaires ou pages du système. Ils ne mesurent pas le RSS global du processus. `clear_cache()` libère le cache décodé et le cache de pages, en gardant le répertoire validé ; `reset_io_stats()` ne vide pas ces caches et conserve ces instantanés de mémoire.

Les index ne contiennent pas les données déchiffrées des fichiers. Les types, IDs, tailles et positions restent visibles comme dans le format précédent.

Les opérations de reconstruction, fermeture de dépendances, ingestion et inventaire gardent une session de lecture. Elle réutilise au plus **8 fichiers d'index ouverts**, avec LRU, en plus des 8 fichiers de données de packs possibles. Les sessions imbriquées partagent ces handles, fermés au retour ou sur exception, et avant le retrait des anciens fichiers. Une recherche isolée hors session ouvre son index pour la page manquante puis le ferme. Cette réutilisation évite de rouvrir le même index à chaque page d'un lot.

## Mémoire et limites

Le répertoire utilise 98 octets par page, plus environ 10 bits par entrée pour le filtre. Pour 1 048 576 entrées d'un pack, les données de répertoire occupent 401 408 octets et le filtre 1 310 720 octets, avant cache et overhead. Le fichier d'index correspondant conserve ses 47 185 936 octets. La construction de cet index écrit aussi les enregistrements par pages ; elle ne prépare plus un buffer de tout le fichier d'index.

Les plafonds restent 1 048 576 objets par pack, 1 024 packs et 4 194 304 objets empaquetés au total. L'ouverture garde un coût linéaire de validation ; elle ne se résume pas à quelques lectures de pages. Les recherches aléatoires peuvent nécessiter davantage d'IO qu'une table entièrement en mémoire, surtout avec un cache petit ou nul. Le filtre réduit les recherches inutiles entre packs sans éliminer tous les faux positifs.

Le GC utilise désormais ces index pour parcourir les objets par pages et marquer les objets empaquetés avec un bit par entrée. Il ne matérialise plus l'inventaire complet, la liste des objets morts ni les listes de survivants par pack. La réécriture et la relecture des packs de remplacement utilisent aussi ce parcours. Chaque page d'énumération est indépendante du cache : un callback peut effectuer des recherches ou lectures imbriquées sans invalider son buffer, même avec un cache nul.

`fsck` utilise également un bitmap et une énumération par type : les bornes des pages sélectionnent les révisions, puis les manifestes, puis les blocs sans constituer un inventaire complet. Les pages aux frontières sont filtrées en conservant les positions d'origine. Voir les [garanties des audits](AUDIT.md).

`objects()`, l'API publique `closure()` et les plans de `pack`/compaction conservent encore leurs collections d'identifiants. Le GC et `fsck` gardent un ensemble compact d'IDs pour les objets parcourus absents des packs, une pile de dépendances et les métadonnées JSON du fichier en cours. Le moteur entier ne dispose donc pas encore d'un plafond global de RAM indépendant de la taille du projet. Voir les [budgets du GC](PACKS.md#mémoire-du-gc) et les [mesures](PERFORMANCE.md).
