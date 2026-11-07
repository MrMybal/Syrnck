---
id: native-amend
title: Modification du dernier commit
description: Guide : Modification du dernier commit.
section: Versionnage
order: 190
parent: native-history
sourceRevision: 1
---

# Modifier le dernier commit natif {#native-commit-amendment}

Amend remplace la tête d’une branche en conservant sa liste ordonnée de parents, y compris tous les parents d’une fusion. L’ancienne révision demeure immuable sous `archive/amend/OLD_ID`. Déplacement de branche, archive et consommation éventuelle de l’index sont publiés atomiquement. Les tags existants gardent leur cible initiale.

## Commandes {#cli}

Tous les modes exigent `-m`/`--message`. La branche sélectionnée est utilisée par défaut ; `--branch NAME` la remplace sans modifier HEAD symbolique. `--expect` est l’ID exact de l’ancienne révision, 64 caractères hexadécimaux minuscules, jamais un sélecteur. Sans cette option, le CLI capture la tête sous verrou exclusif. Il faut une tête existante ; une branche initiale vide ne peut pas être modifiée. `--message-only` et `--expect` nécessitent `--amend`.

Le mode ordinaire réingère le dossier complet avec les exclusions/préférences habituelles et refuse tout index. `--staged` utilise exactement l’index immuable, qui doit exister avec la tête actuelle pour base ; il est consommé atomiquement et aucun fichier de travail n’est lu. `--message-only` reprend toutes les entrées de la tête sans lire le workspace et refuse un index.

Un index sans changement est accepté pour corriger un message. Modes staged/message-only sont exclusifs et refusent les arguments de dossier/pipeline. Le mode dossier accepte `--directory`, `--workers`, `--buffer-mib` et contrôle la liaison au workspace. Tous refusent une récupération checkout/switch en cours. Un index périmé exige inspection et reset explicites.

La nouvelle révision reçoit le message et la date UTC courante en secondes. Le format v1 n’a ni auteur ni signature. Si toute la sérialisation est identique, l’ID existant peut être rendu ; aucun nonce ne force un nouvel ID.

```text
# Replace the tip's snapshot with the current selected directory.
syrnck native commit --amend -m "Corrected project snapshot"

# Use exactly the prepared bytes, leaving later working edits alone.
syrnck native add Content/Maps Source
syrnck native commit --amend --staged -m "Corrected selected snapshot"

# Reuse every existing file entry and change the message.
syrnck native commit --amend --message-only -m "Corrected message"

# Guard scripts against amending a different tip from the one inspected.
syrnck native commit --amend --message-only --expect OLD_ID -m "Correction"
```


## Rétention, interruption et restauration {#retention-interruption-and-restoration}

Avant publication, les fermetures complètes du remplacement et de l’archive sont vérifiées, hashes de fichiers compris. Un ancien bloc absent bloque même un remplacement qui n’en a plus besoin. Les valeurs attendues branche/index/archive sont contrôlées ensemble. Une archive déjà présente n’est réutilisable que si elle pointe vers la même ancienne révision.

Annulation, erreur de callback ou entrée invalide avant publication conservent les anciennes références. La préparation peut laisser des objets immuables non référencés, réutilisables ou collectables. Une coupure pendant publication laisse l’ensemble ancien ou nouveau complet. Après engagement, la publication peut terminer malgré l’annulation ; aucun callback ni contrôle d’arrêt n’intervient après succès.

Après interruption, inspecter `native head`, `refs`, `native staged`. Si l’ancienne tête demeure, réessayer avec son ID attendu exact. Si nouvelle tête et archive sont présentes, la publication est terminée. Un nouvel essai avec l’ancien ID refuse alors de modifier la nouvelle tête une seconde fois.

L’archive protège l’ancienne révision, ses ancêtres et tous ses blocs sans expiration automatique. Exporter son ID ou créer une branche de récupération, puis suivre les règles de checkout/switch pour revenir dans le workspace. Le [retrait protégé par sauvegarde](doc:archive-retention) permet un nettoyage explicite. Retirer la dernière racine rend les données exclusives collectables ; la rétention locale n’est pas une frontière d’autorisation.

```text
syrnck native export OLD_ID recovered-version
syrnck native branch recovered OLD_ID
```


## API C++ {#c-api-and-validation}

Chaque API exige l’ID exact attendu. La surcharge dossier accepte `IngestOptions`, avec préférences du dépôt par défaut ; staged/message acceptent `OperationOptions`. Les callbacks ne doivent pas réentrer dans le Store. Le choix de snapshot peut être fondé sur les métadonnées, mais la publication vérifie toujours les payloads complets. Progression anglaise/française et Ctrl+C coopératif sont disponibles ; JSON et IDs restent indépendants de la langue. Ces opérations d’historique ne sont pas encore exposées en C/Go.

```cpp
auto expected = store.resolve_native_revision("heads/main");
auto corrected = store.amend_directory(project, "main", expected, "Corrected snapshot");
auto prepared = store.amend_staged("main", corrected, "Selected correction", operation);
auto renamed = store.amend_message("main", prepared, "Corrected message", operation);
```
