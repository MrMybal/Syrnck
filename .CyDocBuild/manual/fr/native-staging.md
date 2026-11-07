---
id: native-staging
title: Index de préparation et commits sélectionnés
description: Guide : Index de préparation et commits sélectionnés.
section: Versionnage
order: 380
parent: native-history
sourceRevision: 1
---

# Préparation native et commits sélectionnés {#native-staging-and-selected-commits}

Le moteur autonome fige des fichiers choisis, inspecte le snapshot, retire des sélections puis commite sans relire le workspace. Cela concerne les branches natives ; Git garde son propre index. add . sélectionne tout le candidat ; reset sans chemin vide l’index. Aucun fichier de travail n’est modifié. Branche courante par défaut, main pour les anciens Stores ; --branch en choisit une autre sans changer HEAD.

```text
syrnck native add Source Content/Maps --branch main --progress
syrnck native staged --branch main
syrnck native reset Source --branch main
syrnck native commit --staged -m "Update the selected maps" --branch main
```


## Sélection et octets figés {#selection-and-frozen-contents}

Un chemin sélectionne un fichier exact ou tout un préfixe de dossier, relatif à --directory (défaut .), pas à une racine Git. Le CLI normalise séparateurs, ./ et slash final ; C++ attend des chemins validés ou . Wildcards, absolus et sorties de racine sont refusés ; -- permet un chemin commençant par tiret.

Chaque add modifie l’index existant, initialement copié de la branche. Les entrées non choisies gardent leurs octets préparés ; les fichiers choisis sont ingérés immédiatement. Un suivi/préparé absent de la sélection actuelle est retiré, y compris exclu par ignore. Aucun match côté travail ou base/index est une erreur ; les dossiers vides non suivis n’ont pas d’entrée. Sélections répétées/chevauchantes autorisées ; dir ne prend pas dir 2. Pour une transition de type/casse qui laisserait des entrées incompatibles, choisir leur parent commun.

Scanner/exclusions identiques au commit/statut : ignores imbriqués, chemins/casse portables et fichiers ordinaires. Le Store est intérieur. L’ajout visite encore le candidat entier et n’ingère que les fichiers sélectionnés : un chemin non choisi invalide peut faire échouer. Il ne s’autorise pas sur le cache consultatif du watcher. Scan et métadonnées plates gardent leurs plafonds de 16 Mio.

Add rend l’ID immuable du snapshot complet, voisins inchangés compris, inspectable/exportable. Même base/entrées donne même ID. Les modifications ultérieures du disque ne changent pas les préparations ; refaire add remplace explicitement les octets choisis.

## Inspecter, réinitialiser et commiter {#inspect-reset-and-commit}

native staged ne lit que révisions/manifestes et expose branch, base enregistrée ou null, head actuelle ou null, snapshot ou null, stale si base différente de tête, clean si entrées égales à la base, staged_files total et counts/changes. Les lignes distinguent contenu, mode et manifeste ; une représentation seule peut être préparée. Sans index, rapport propre contre la tête ou base vide initiale. L’index périmé reste inspectable sur son ancienne base ; cela n’audite pas les payloads, contrairement à publication/GC.

Status ordinaire compare branche/disque ; --revision STAGING_ID compare à l’index ; --combined rend les deux côtés avec staleness. Reset PATH rétablit les entrées de la base enregistrée ou retire les nouveaux ajouts, sans lire/restaurer le disque et en gardant un index explicite même propre. Reset sans chemin retire tout l’index, de façon idempotente.

Commit --staged publie exactement l’index comme enfant de sa base et le consomme atomiquement ; branche initiale = commit racine. Il exige des entrées changées, accepte progression/arrêt mais refuse dossier/pipeline puisqu’il ne lit pas le workspace. Le commit ordinaire du dossier refuse tout index existant : commiter ou retirer la sélection auparavant.

Branche déplacée/retirée rend l’index périmé et bloque add, reset sélectif et commit staged. Inspecter/exporter puis reset complet, ou [rejouer l’index](doc:native-stage-rebase) sur une tête existante, avec archivage et refus des conflits. [Hunks](doc:native-hunks), [lignes](doc:native-lines) et [patches externes](doc:text-patches) permettent la préparation partielle exacte ; l’éditeur de patch interactif reste à développer.

## Rétention et publication {#retention-and-publication}

staging/BRANCH pointe une révision v1 ordinaire avec date fixe, marqueur lié à la branche et parent de base facultatif. C’est une table complète, pas une chaîne d’éditions ; heads/BRANCH ne bouge qu’au commit. Le snapshot interne ne devient pas un parent de commit utilisateur.

La racine conserve tous les manifestes/blocs préparés et l’histoire de base contre le GC, même sans branche visible, sur tous les stockages et avec chiffrement habituel. Ne pas retirer manuellement staging pour un index actif.

Add/reset sélectif vérifient la fermeture du remplacement ; commit staged vérifie puis publie branche et retrait d’index ensemble par refs.json atomique, avec attentes branche/index contrôlées. Reset complet retire uniquement l’index. Verrou exclusif pendant toute l’opération ; aucun callback/stop après publication.

Annulation, mutation source, chemin/dépendance invalide ou callback en échec avant publication gardent l’ancienne branche/index. Des objets préparés immuables peuvent rester sans référence, réutilisables ou collectables. Aucun rollback de fichiers puisque staging n’en écrit pas. Le scan n’est pas un snapshot atomique : les fichiers sont contrôlés autour de l’ingestion et une mutation concurrente peut demander un nouvel essai.

## API C++ {#c-api-and-validation}

Amend --staged réutilise le même index, même propre pour corriger le message. Il conserve les parents de l’ancienne tête et archive cette dernière en consommant l’index atomiquement. Voir [amend](doc:native-amend).

stage_directory possède une surcharge IngestOptions ; le défaut utilise les préférences d’ingestion. Les autres opérations acceptent OperationOptions. Callbacks synchrones non réentrants, progression scan/ingestion puis vérification et dernier publishing/refs. Les mutations d’historique natives restent hors bindings C/Go.

```cpp
auto snapshot = store.stage_directory(project, "main", {"Source", "Content/Maps"});
auto report = store.staged_status("main", operation);
auto remaining = store.reset_stage("main", {"Source"}, operation);
auto commit = store.commit_staged("main", "Selected changes", operation);
store.reset_stage("main");
```


## Inspecter le texte préparé {#inspect-prepared-text}

native diff-text --staged PATH compare de façon vérifiée et bornée le fichier figé à la base originale de son index. Un index périmé garde sa base et les modifications ultérieures du workspace ne sont pas lues. --unified rend le patch ; voir le [diff textuel](doc:text-diff) et la [sélection de hunks](doc:native-hunks).
