---
id: git-lfs-migration
title: Migration d’un snapshot LFS
description: Guide : Migration d’un snapshot LFS.
section: Git
order: 140
parent: git-assets
sourceRevision: 1
---

# Migration hors ligne d’un snapshot LFS {#offline-git-lfs-migration}

Un snapshot commité contenant des assets LFS peut être converti en pointeurs Syrnck dans un commit enfant sur une branche neuve. Source, HEAD, index et modifications de travail restent intacts. Les commits antérieurs gardent leurs pointeurs LFS et leur stockage nécessaire. Pour eux, utiliser la [conversion complète d’historique](doc:git-lfs-history). Téléchargement d’objets LFS et pipelines d’extensions LFS ne sont pas implémentés.

## Préparer et inspecter {#prepare-and-inspect}

Rendre les objets LFS disponibles localement avec les outils LFS habituels ou une copie hors ligne. Syrnck ne contacte pas LFS et n’exécute pas git-lfs. L’aperçu lit les blobs commités via Git, sans Store, clé ni checkout ; un clone --no-checkout suffit côté Git.

Le cache par défaut est lfs/objects du dossier commun Git, partagé par worktrees. Un objet utilise OBJECTS_DIRECTORY/OID[0:2]/OID[2:4]/OID. Pour un cache copié ou lfs.storage personnalisé, fournir explicitement son dossier objects ; aucune interprétation de ce réglage n’est devinée. Un chemin relatif part du répertoire du processus.

Le JSON liste chemin, blob Git, OID SHA-256 LFS, taille, mode exécutable et statut verified/missing/size_mismatch/hash_mismatch. Les OID dupliqués ne sont hashés qu’une fois. ready exige un catalogue non vide dont tous les objets sont vérifiés ; code 0 si prêt, 1 sinon. Pointeurs mal formés et entrées dangereuses échouent. Racine du cache, deux dossiers de shards et fichiers ne doivent pas être des symlinks ; les ancêtres au-dessus de la racine fournie ne sont pas contrôlés.

Cet aperçu audite le contenu LFS, pas la possibilité finale d’utiliser branche, attributs, identité auteur, assets Syrnck ou hooks.

```text
syrnck git lfs-preview HEAD --progress
```


```text
OBJECTS_DIRECTORY/OID[0:2]/OID[2:4]/OID
```


```text
syrnck git lfs-preview HEAD --directory PROJECT --lfs-objects CACHE/objects
```


## Convertir le snapshot {#convert-a-snapshot}

Initialiser Store/filtres puis choisir une branche inutilisée. git install configure les filtres séparément ; la migration ne change pas la configuration et n’installe pas de hook. Il faut Store existant, identité Git locale et tous les objets LFS. Les options de dossier/cache et préférences d’ingestion s’appliquent comme à l’aperçu.

Chaque objet unique passe en flux par découpage, compression et chiffrement optionnel. SHA-256 porte sur les octets réellement consommés ; taille et hash doivent correspondre au pointeur. Manifestes/blocs gardent leurs IDs BLAKE3 et formats ; les doublons réutilisent le manifeste.

Seuls pointeurs sélectionnés et attributs nécessaires changent dans l’arbre Git. Chaque chemin reçoit dans son parent immédiat une règle de basename ancrée littérale ajoutée après les règles existantes, avec espaces/crochets échappés. Elle remplace LFS et désactive les transformations texte. Les autres fichiers/sous-arbres, modes, liens et sous-modules gardent leurs IDs. Seuls les pointeurs de fichiers ordinaires sont convertis. Les attributs modifiés doivent être ordinaires, sans NUL et sous leurs limites ; un .gitattributes lui-même sous LFS n’est pas convertible.

Avant la branche, vérifier tout le catalogue natif du nouveau commit, y compris les anciens assets Syrnck, et le retenir sous git-assets/NEW_COMMIT. Git prépare ensuite une transaction et tient le verrou de référence, sous lequel Syrnck revérifie le nom. Cela protège aussi les références symboliques pendantes sur les anciennes versions de Git. Référence existante ou branche sélectionnée par HEAD initial/worktree est refusée. Les hooks reference-transaction peuvent rejeter ; commit-tree ne lance pas les hooks ordinaires de commit ni signature.

Le succès rend source, commit, branch, converted_assets, unique_objects, bundle, assets et reconciled. Examiner puis sélectionner la branche explicitement. Upload de blocs et publication Git restent séparés. Le cache LFS reste en lecture seule et en clair même si le Store cible est chiffré.

```text
syrnck git install
syrnck git migrate-lfs HEAD --branch converted-assets --workers 4 --buffer-mib 32 --progress
git diff HEAD converted-assets
```


```text
"/Map.uasset" filter=syrnck -text -diff -merge -ident -working-tree-encoding -eol -crlf
```


## Annulation et récupération {#cancellation-and-recovery}

Progression texte/JSON sur stderr ; Ctrl+C coopératif rend 130 avant engagement. Commandes Git enfants, hooks et lectures de pipe bloquées peuvent retarder l’arrêt.

Après démarrage du commit Git final, publication ou réconciliation se termine sans nouveau contrôle d’annulation. Un arrêt tardif peut réussir. En cas d’accusé perdu, une référence égale exactement au commit préparé rend reconciled true ; sinon code 3 et inspection avant tout nouvel essai. Ne pas déduire l’absence de publication d’une réponse perdue.

Avant pin de bundle, une erreur peut laisser des objets valides collectables. Après ce pin, les blocs restent protégés même sans branche. Git peut aussi garder blobs/arbres/commit inaccessibles. Aucun journal ni suppression automatique de pins préparés ; inspecter refs, référence Git et catalogue avant retrait volontaire. Réessayer réutilise les objets mais exige une branche cible absente. Le cache LFS n’est jamais supprimé.

## Bibliothèque et limites {#library-and-limits}

`syrnck_git` et git_lfs.hpp exposent parsing, catalogue, résolution du cache, aperçu et migration sans HTTP. L’appelant fournit un Store ouvert et éventuellement IngestOptions ; callbacks selon le contrat d’opération. SHA-256 vient de PicoSHA2.

Bornes : arbre Git 64 Mio et 262144 entrées, dossiers compris ; candidats pointeurs ordinaires au plus 1024 octets, lots de 128 et réponse 160 Kio ; pointeurs LFS canoniques strictement inférieurs à 1024 octets, versions anciennes/extensions/champs inconnus refusés. Migration : 65536 chemins d’assets, 4096 dossiers d’attributs modifiés, 1 Mio par fichier d’attributs et 16 Mio au total, bundle natif aussi limité à 16 Mio.

SHA-256 travaille par tampons de 64 Kio et les assets via le pipeline borné, sans copie temporaire complète ; arbres, IDs, pointeurs et tables de changements croissent avec le snapshot. Lignes de réponse de transaction : 4 Kio ; diagnostic d’accusé : 16 Kio. Stderr Git est temporaire, sans quota propre de taille ou durée des hooks. Sémantique des pointeurs et écritures d’arbres/références suivent Git LFS et Git.
