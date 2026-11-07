---
id: native-comparison
title: Statut et comparaison de snapshots
description: Guide : Statut et comparaison de snapshots.
section: Versionnage
order: 240
parent: native-history
sourceRevision: 1
---

# Statut du dossier et comparaison des snapshots {#native-working-directory-status-and-snapshot-comparison}

Le moteur inspecte un workspace et compare deux snapshots sans importer de fichiers ou publier de références. Résultats JSON anglais canoniques ; progression et diagnostics anglais/français avec les options habituelles.

```text
syrnck native status
syrnck native status --branch feature --directory PROJECT
syrnck native status --revision REVISION --progress-json
syrnck native status --combined --branch main
syrnck native diff SOURCE_REVISION TARGET_REVISION
```


## Statut du dossier de travail {#working-directory-status}

L’option [--indexed](doc:working-index) possède un contrat de réutilisation distinct et peut écrire son cache. Sans elle, le statut lit intégralement les contenus sans écrire.

La base par défaut est la branche native courante, main implicitement pour les anciens Stores. --branch choisit une autre branche ; absente, elle forme une base vide et les fichiers sont non suivis. --revision choisit une révision explicite valide et exclut --branch. --combined rend le schéma séparé base/index/workspace et exclut --revision ; le statut ordinaire compare branche et disque.

Chaque fichier ordinaire sélectionné est lu et hashé en BLAKE3, même avec taille/date identiques. Champs : base, clean, tracked_files, workspace_files, hashed_bytes, counts, changes triés. Un résultat propre compare les octets aux hashes de manifestes sans auditer leurs blocs stockés.

Statuts : untracked sans entrée de base ; removed absent de la sélection de travail ; modified pour contenu ou mode différent ; unchanged dans les compteurs. Chaque changement donne path/status/source/target/content_changed/executable_changed, côtés absents null. Résumés : manifest/size/file_hash/executable ; manifeste de travail null car aucun import. Ajout/retrait implique content_changed ; mode comparé seulement avec deux côtés. Tout changement, non-suivis compris, rend clean false ; le code reste 0 si la requête réussit.

Le scanner partagé avec commit applique ignores imbriqués et négations, exclusions .git/.syrnck/Store, chemins portables et collisions de casse. Le Store doit être intérieur au dossier ; les ignores sont des fichiers ordinaires ; liens et fichiers spéciaux sélectionnés sont refusés et les exclus ne sont pas suivis.

Les ignores s’appliquent même aux suivis : ignorer un fichier suivi le signale removed et l’exclut du prochain commit ordinaire, même encore présent sur disque. L’index natif peut figer les entrées indépendamment. Unix inspecte le bit exécutable ; Windows enregistre false et ignore les différences exécutables locales avec un snapshot Unix. Le diff de snapshots continue à les signaler partout.

## Comparer deux révisions {#revision-snapshot-comparison}

native diff lit seulement les métadonnées vérifiées des révisions et de tous leurs manifestes, inchangés compris, sans blocs ni parcours des parents. Il peut inspecter un Store partiel de métadonnées ; seul un audit complet prouve la disponibilité des payloads/ancêtres.

Le CLI accepte IDs, HEAD et noms non ambigus, ou heads/NAME/tags/NAME. Les méthodes C++ reçoivent des IDs ; la résolution reste séparée.

Champs : source, target, equal, source_files, target_files, counts, changes triés. Statuts added/removed/modified et compteur unchanged. Résumés identiques au statut, avec manifest_changed. Un autre découpage peut décrire les mêmes octets : modified avec manifeste changé mais contenu identique. Les modes sont distincts ; messages, dates et parents n’affectent pas l’égalité des entrées. Sans option, un renommage reste suppression/ajout ; [--renames](doc:native-file-history) détecte les correspondances exactes. [Diff textuel](doc:text-diff), [patches](doc:text-patches), fusion et checkout ont leurs opérations propres.

## API et frontières d’observation {#c-and-operation-boundaries}

En C++, baseline_revision est optionnel ; sans lui la base est vide, la sélection de branche appartenant au CLI. Les [sessions C/Go](doc:workspace-abi) fournissent les statuts de workspace ; les mutations générales d’historique restent séparées.

Callbacks synchrones non réentrants : scanning pour dossiers, verifying pour octets des fichiers, planning pour comparaisons ; compteurs remis par phase. Annulation/exception : aucun rapport partiel ni changement. JSON écrit au succès seulement ; verrou exclusif et récupération normale restent requis.

Tampon 1 Mio, FIFO de 1024 résumés de manifeste, snapshots et rapport complets en mémoire, objets de 16 Mio maximum. Le scanner refuse un snapshot plat dont la borne minimale dépasse déjà 16 Mio ; ce n’est pas un plafond global de RAM.

Taille/date/type/mode Unix sont contrôlés autour de la lecture, croissance comprise, sans verrou de workspace ni snapshot atomique. Les modifications concurrentes peuvent imposer un nouvel essai et celles postérieures périmer le statut. [Index de travail](doc:working-index) et [observateur](doc:native-watch) accélèrent des observations avec leurs propres limites, sans remplacer ces garanties.

```cpp
auto status = store.status_directory(project, baseline_revision, operation);
auto empty_baseline = store.status_directory(project);
auto difference = store.compare_revisions(source_id, target_id, operation);
```
