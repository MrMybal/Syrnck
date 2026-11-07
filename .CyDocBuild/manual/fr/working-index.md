---
id: working-index
title: Index de travail persistant
description: Guide : Index de travail persistant.
section: Stockage
order: 630
parent: storage
sourceRevision: 1
---

# Index de travail persistant {#persistent-native-working-index}

native status --indexed active un cache local consultatif de hashes vérifiés, évitant de relire les assets inchangés lorsque le système de fichiers fournit identité et métadonnées nécessaires. Le statut ordinaire continue à tout hasher. Le mode combined garde les comparaisons branche/index/workspace, y compris un index périmé. Ce cache décrit les fichiers observés ; il est distinct de l’index immuable de préparation et ne prépare/commite jamais de contenu. JSON anglais dans les deux langues.

```text
syrnck native status --indexed
syrnck native status --indexed --combined --progress-json
syrnck native status --indexed --revision REVISION
syrnck native index-clear
```


## Réutilisation et repli {#reuse-and-fallback}

Une entrée contient chemin, taille, hash complet BLAKE3, mode et digest d’identité/métadonnées filesystem. Des handles interrogent les métadonnées avant/après observation. Réutilisation seulement si identité, mtime, ctime, taille et mode/attributs correspondent à l’observation vérifiée. Dates récentes ou futures contournent le cache avec une garde prudente de deux secondes ; écritures remettant taille/mtime et remplacement de fichier restent détectables par les autres champs.

Windows : volumes locaux fixes NTFS/ReFS, identité de volume et fichier 128 bits, dates d’écriture/changement et attributs. Linux : familles ext, XFS, Btrfs, tmpfs, device/inode, mtime/ctime nanoseconde et mode. macOS : APFS local avec les mêmes champs. Autres plateformes/filesystems : hash complet.

UNC/réseau, filesystem inconnu, échec d’identité et écriture récente repliquent sur lecture complète. Sous Windows, les handles de métadonnées refusent le partage en écriture : un writer actif pouvant différer ses dates ne peut réutiliser un ancien hash. Une identité changeant pendant observation fait échouer et impose un nouvel essai. Ignores, refus de liens/reparse et namespace portable restent appliqués.

C’est une accélération stat fondée sur la confiance du cache local, pas un audit ni snapshot atomique. Falsification délibérée des métadonnées, horloge manipulée et remplacements arbitraires concurrents sont hors hypothèses. Le statut ordinaire compare indépendamment les octets. Checkout/reset/stash/commit/staging gardent leurs lectures complètes et ne s’autorisent jamais à remplacer un fichier sur ce cache.

## Persistance et durée de vie {#persistence-and-lifetime}

Le pointeur working-index.json du Store lie un chunk CAS à l’empreinte de la racine canonique du workspace. Le chunk d’index est compressé et chiffré avec les objets habituels ; le pointeur ne contient que des IDs. Il est remplacé atomiquement après scan complet et dernier observateur. Une mutation du pointeur par callback bloque la publication au lieu de l’écraser. L’annulation peut laisser un chunk non référencé mais jamais un pointeur partiel.

Ce cache jetable n’est pas une racine de versions. Le GC peut collecter ses objets ; sauvegarde/réplication logiques n’en dépendent pas. Objet absent, enveloppe corrompue ou document mal formé entraînent un nouveau scan/cache. Un pointeur lié, non ordinaire ou trop grand est refusé plutôt qu’écrasé. index-clear retire seulement le pointeur ; le GC reprend ensuite les chunks. Un ancien objet abîmé n’est pas réparé et reste visible à l’audit complet jusqu’à sa collecte.

Un workspace mis en cache par Store ; une autre racine canonique le reconstruit. Objet limité à 16 Mio : si trop d’entrées, le scan reste complet mais seule une partie est mémorisée, avec truncated true. Snapshots, maps, JSON et autres allocations s’ajoutent à ce budget.

## Rapports et API C++ {#reports-c-and-measurement}

Les rapports gardent leurs champs et ajoutent working_index : previous = missing/ready/invalid/other_workspace ; hashed_files/reused_files ; supported_files (identités stables, même trop récentes pour réutilisation) ; cached_files/truncated/object. hashed_bytes compte seulement les octets de travail lus, zéro pour une réutilisation, hors métadonnées stockées. Le mode indexé peut écrire et émettre publishing ; l’ordinaire reste en lecture seule.

Chaque requête indexée énumère encore dossiers et ignores pour voir ajouts/retraits/renommages/règles. L’[observateur résident](doc:native-watch) utilise notifications et audits avec indices bornés et récupération de débordement. Son inventaire complet en session permet de remplacer un fichier/sous-arbre pour éditions, créations, retraits, renommages, types et ignores imbriqués sans énumérer ailleurs. Règles racine, perte de continuité et sélection ambiguë imposent une relecture complète des octets.

Linux étend les inscriptions de dossiers créés/importés avant les indices ciblés ; retraits/déplacements d’anciens dossiers observés imposent encore un scan complet pour couvrir les trous inotify. Un checkpoint persistant est optionnel ; historique rejouable d’événements et réconciliation incrémentale native des retraits/renommages restent distincts. Les [sessions C/Go](doc:workspace-abi) offrent cette observation native/polling et conservent les indices pendant les attentes, avec réouverture de Store à chaque requête. Les gains dépendent des fichiers et du filesystem ; aucun débit de corpus Unreal n’est garanti.

```cpp
#include <syrnck/working_index.hpp>
auto report = syrnck::indexed_native_status(store, project, revision, operation);
auto combined = syrnck::indexed_native_workspace_status(store, project, "main", operation);
auto cleared = syrnck::clear_native_working_index(store, operation);
```
