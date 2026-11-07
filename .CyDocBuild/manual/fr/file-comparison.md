---
id: file-comparison
title: Comparaison de manifestes
description: Guide : Comparaison de manifestes.
section: Stockage
order: 90
parent: storage
sourceRevision: 1
---

# Comparer les manifestes et préparer la réutilisation {#manifest-comparison-and-chunk-reuse-plans}

`Store::compare_files(source_manifest, target_manifest, operation)` compare deux manifestes v1 vérifiés sans lire les payloads. Le JSON anglais ordonné contient `version: 1`, IDs/tailles source et cible, `same_content`, `reused_bytes`, `new_bytes`, `unique_new_bytes`, `blocks`, `needed_chunks`.

`--summary` retire les deux listes de blocs de stdout après calcul : cela réduit la sortie, pas la mémoire de planification. La progression reste sur stderr, en texte anglais/français ou JSON stable. Aucune référence ne change.

L’ABI C `syrnck_file_compare_json` utilise un tampon de l’appelant et une requête de taille comprenant NUL. `CompareFiles` en Go rend `FileComparison`, `FileBlock`, `NeededChunk` et borne le JSON à 128 Mio avant décodage. La requête de taille suivie de la lecture calcule deux fois le plan ; aucun pointeur natif n’est conservé. Utiliser en-tête et bibliothèque de la même version.

Chaque bloc cible expose `id`, `offset` cible, `size`, et `source_offset`, première occurrence correspondante dans la source ou null. Les blocs déplacés/répétés sont réutilisables ; un même ID doit avoir partout la même taille déclarée. `needed_chunks` cite chaque bloc absent une seule fois, dans l’ordre de première apparition cible.

`reused_bytes + new_bytes` égale la taille cible. `new_bytes` compte chaque occurrence ; `unique_new_bytes` compte une seule fois chaque bloc absent distinct. Ce sont des octets logiques en clair, sans manifestes, protocole, chiffrement ni compression, pas une mesure réseau. Un destinataire déjà rempli peut encore réduire les besoins.

`same_content` compare taille et hash complet indépendamment du découpage : des octets identiques peuvent n’offrir aucune frontière commune. Le plan ne cherche pas à l’intérieur des blocs et ne génère pas de delta binaire. C’est une recette de reconstruction, sans modification de format ou référence.

Les blocs peuvent être absents : le plan reste une lecture de métadonnées. Un offset ne prouve pas qu’un fichier mutable conserve ces octets. Vérifier les blocs copiés/importés contre leurs IDs et le fichier terminé contre le manifeste avant publication. Les API de lecture, import et publication vérifiée fournissent ces contrôles.

Un seul manifeste est lu pour deux IDs égaux, sinon deux. Métadonnées et résultat croissent avec les blocs, sous la limite de 16 Mio par manifeste. Coût de recherche : O((blocs source + cible) log(IDs distincts)), sans tampon à la taille du fichier. Il n’existe pas encore d’index de manifeste persistant.

La phase `planning` compte les entrées traitées dans les deux manifestes, pas les octets de fichiers. Le contrat d’[opération](doc:operations) régit callbacks et annulation. Aucun pin n’est créé : l’appelant doit conserver ses racines de rétention.

```text
syrnck compare SOURCE_MANIFEST TARGET_MANIFEST --progress-json
syrnck compare SOURCE_MANIFEST TARGET_MANIFEST --summary
```
