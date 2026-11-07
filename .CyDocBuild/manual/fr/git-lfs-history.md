---
id: git-lfs-history
title: Conversion de tout l’historique LFS
description: Guide : Conversion de tout l’historique LFS.
section: Git
order: 130
parent: git-assets
sourceRevision: 1
---

# Convertir tout l’historique LFS accessible {#offline-conversion-of-complete-git-lfs-history}

`syrnck_git` convertit tous les commits accessibles depuis une tête, y compris chaque parent de fusion, sur une nouvelle branche. Références source, HEAD, index et workspace restent intacts. Aucun serveur LFS ni processus git-lfs n’est utilisé. Les deux commandes acceptent `--directory PROJECT`, `--lfs-objects CACHE/objects`, `--max-commits 10000`, `--strip-signatures`. L’ingestion reprend les préférences du Store sauf substitution ; installer les filtres séparément avec git install.

```text
syrnck git lfs-history-preview main --progress
syrnck git migrate-lfs-history main --branch converted-history --workers 4 --progress
git log --graph converted-history
```


## Contrôle préalable complet {#complete-preflight}

Le parcours lit les parents bruts, sans remplacements ni simplification. Tout ancêtre manquant échoue ; une limite shallow n’est jamais une nouvelle racine implicite. Le sélecteur est résolu une fois en commit immuable, indépendant d’un déplacement ultérieur de branche.

Avant import, chaque arbre source distinct est vérifié pour pointeurs LFS/attributs, chaque objet de cache distinct est audité en SHA-256 et les dépendances Syrnck existantes sont vérifiées. Même un asset historique supprimé du dernier snapshot exige son cache. Objet ancien absent, tronqué ou corrompu bloque avant toute écriture de candidat ou Store. Les octets effectivement ingérés sont revérifiés ensuite pour détecter une mutation tardive du cache.

L’aperçu ne demande ni Store ni clé. Il rend statut des objets, parents/arbre de chaque commit, compteurs et nécessité de retirer les signatures. Comme l’aperçu de snapshot, il audite le cache LFS, pas la disponibilité des assets Syrnck ni la transaction finale de branche.

## Historique et signatures {#preserved-history-and-signatures}

La conversion garde parents ordonnés, fusions, racines, commits vides, en-têtes bruts auteur/committer avec dates/fuseaux, encoding, messages et extensions acceptées. Les commits inchangés gardent leurs IDs. Les arbres modifiés utilisent la conversion de fichiers ordinaires et les attributs littéraux de la [migration de snapshot](doc:git-lfs-migration). Code inchangé, modes exécutables, blobs de liens et gitlinks conservent leurs objets.

Un commit réécrit ne peut garder une signature devenue fausse. Par défaut, tout gpgsig, gpgsig-sha 256 ou mergetag d’un commit à réécrire bloque. Après examen de l’aperçu, autoriser explicitement leur retrait avec `--strip-signatures`. Ces en-têtes et leurs continuations ne sont retirés que des commits réécrits et sont comptés ; les commits signés inchangés restent exacts. Aucun resignage, changement de tags ou d’autres branches n’est automatique. Répéter sur d’autres têtes si nécessaire : les objets se dédupliquent, mais références/signatures demandent leur propre politique.

```text
syrnck git lfs-history-preview main --strip-signatures
syrnck git migrate-lfs-history main --branch converted-history --strip-signatures
```


## Rétention et publication {#retention-and-publication}

Chaque commit historique converti possédant des assets Syrnck reçoit un catalogue complet vérifié sous `git-assets/COMMIT` avant publication de la branche. Une suppression ultérieure d’asset, le nettoyage Git ou le GC/empaquetage Syrnck ne retire donc pas ces versions. Les assets déjà natifs sont inclus ; un catalogue vide ne demande aucune racine Store.

La branche neuve est créée par transaction Git préparée avec contrôles de référence symbolique/worktree et hooks de la migration simple. Aucune branche existante n’est remplacée. Annulation avant publication ; après engagement, une observation exacte du commit permet de réconcilier un accusé perdu. Sinon code 3 et inspection de la référence/du commit annoncés.

Une erreur peut laisser des objets valides non référencés et des pins de catalogues déjà préparés, réutilisables, sans journal de migration qui les supprime. Il n’y a pas de transaction distribuée Git/Store : la protection précède la branche. Upload des blocs et publication Git restent séparés.

Le JSON de succès contient source sélectionnée, nouvelle tête/branche, compteurs, `stripped_signature_headers`, `reconciled` et `commits`, table source vers commit/arbre/bundle convertis. Conserver cette correspondance pour revue ou migration séparée des tags. Le cache LFS n’est jamais retiré.

## Bornes {#bounds-and-validation}

Défaut 10000 commits, réglable 1..100000 ; 1024 parents par commit ; 1 Mio par commit brut ; 64 Mio pour octets bruts et estimation des nœuds, avec conteneurs/allocateur en supplément. Au plus 65536 objets LFS distincts, plus les bornes d’arbre/attributs de chaque snapshot. Aperçu et résultat restent sous 16 Mio. Payloads en flux dans le pipeline borné ; arbres, graphe et correspondances en mémoire. Les sous-processus Git sont encore lancés par commit/arbre : c’est un outil hors ligne, pas un serveur optimisé de grands historiques. Les formats de commits et transactions restent ceux de Git.
