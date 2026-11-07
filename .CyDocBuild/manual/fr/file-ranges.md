---
id: file-ranges
title: Lecture vérifiée de plages
description: Guide : Lecture vérifiée de plages.
section: Stockage
order: 100
parent: storage
sourceRevision: 1
---

# Lire des plages vérifiées {#verified-file-ranges}

Les méthodes C++ lisent une plage depuis un manifeste sans reconstruire le fichier entier. Le CLI offre le même export exclusif ; `--offset` et `--length` doivent être fournis ensemble. Sans les deux, la reconstruction complète habituelle s’applique. Avec `--progress-json`, stdout reste vide et stderr rend des compteurs relatifs à la plage.

L’ABI ajoute `syrnck_file_read_range` et `syrnck_restore_file_range` ; utiliser les en-têtes/binaires correspondants. Go expose `ReadRange` et `RestoreRange`. Les lectures tamponnées C/Go sont plafonnées à 16 Mio et préparent les octets avant copie : une erreur ne modifie pas le tampon appelant. Les exports de fichiers en flux n’ont pas cette limite de longueur ; les limites de bloc et de manifeste restent valables.

Une requête C `(NULL, 0)` contrôle métadonnées et bornes sans payload. Un pointeur non nul demande une vraie vérification, même pour une plage vide d’un fichier vide. Une capacité insuffisante rend la longueur nécessaire ; les autres erreurs remettent la longueur à zéro. Go alloue la plage demandée et C ajoute un tampon privé de même taille. Les bornes sont contrôlées avant allocation ; le manifeste est relu pendant la reconstruction.

Offset et longueur sont des comptes d’octets non signés sur 64 bits. La plage doit tenir exactement dans le fichier, sans troncature ni débordement. Une longueur nulle est permise jusqu’à EOF inclus. La destination contient uniquement les octets choisis, à partir de son offset zéro.

Après validation du manifeste, seuls les blocs intersectés sont chargés et entièrement vérifiés : authentification/décompression si nécessaire, ID et taille réelle avant émission. Les blocs non concernés peuvent manquer ; leur disponibilité/intégrité n’est pas auditée.

Une lecture partielle ne valide pas le hash complet du fichier. Une plage couvrant tout le fichier, même vide, utilise la reconstruction normale et valide aussi ce hash. Conserver une reconstruction ou fermeture complète lorsque toutes les dépendances doivent être auditées.

Un flux peut avoir émis un préfixe si une erreur de bloc, callback ou écriture survient ensuite. L’export vers un fichier prépare un frère temporaire et ne publie qu’après succès, sans remplacer une destination existante. Annulation, bornes invalides et blocs concernés corrompus ne publient pas de destination.

La progression `reconstructing` compte les octets de la plage, avec `total_bytes` égal à sa longueur. Callbacks, annulation, verrou exclusif et sérialisation des handles suivent le [contrat d’opération](doc:operations).

Les manifestes v1 et les stockages isolés/mono-pack/multi-packs restent compatibles. Le manifeste complet est analysé pour trouver la plage ; aucun index d’offsets persistant ou manifeste paginé n’est ajouté. La mémoire des payloads C++ reste bornée à un bloc décodé sélectionné, en plus des métadonnées, codecs et caches. Même quelques octets à une frontière peuvent imposer le décodage d’un bloc entier.

```cpp
store.reconstruct_range(manifest, output_stream, offset, length, operation);
store.restore_file_range(manifest, new_destination, offset, length, operation);
```


```text
syrnck get MANIFEST NEW_FILE --offset 1048576 --length 65536 --progress
```
