---
id: native-workspace
title: Inspection du workspace
description: Guide : Inspection du workspace.
section: Versionnage
order: 420
parent: native-history
sourceRevision: 1
---

# Inspection du workspace et aperçu de checkout {#native-workspace-inspection-and-checkout-preflight}

Le statut combiné branche/index/workspace et le plan de checkout ordinaire sont en lecture seule : aucune modification de fichier, objet, référence ou préparation. Un plan n’exécute rien ; le [checkout récupérable](doc:native-checkout) prépare les sorties vérifiées, revérifie le workspace et journalise les déplacements avant publication.

Le CLI utilise la branche native courante, main par défaut historique, et le dossier courant. Le Store doit se trouver dans le workspace, strictement sous sa racine pour le checkout. Un HEAD explicite lie les commandes à la racine canonique. Clés JSON, actions et motifs de conflit restent anglais.

```text
syrnck native status --combined --branch main --progress
syrnck native checkout-plan TARGET_REVISION --branch main --progress-json
syrnck native checkout-plan TARGET_REVISION --verify
```


## Statut combiné {#combined-status}

--indexed active facultativement le [cache persistant de hashes](doc:working-index) côté non préparé ; ce mode peut écrire des données jetables. Sans lui, le statut garde ses lectures complètes sans écriture.

--combined rend version, branch, clean, staged, unstaged. Staged compare la base enregistrée à l’index figé ; unstaged compare cet index au workspace. Sans index, comparer à la tête ; branche initiale vide, base vide.

Un chemin peut apparaître des deux côtés : préparer des octets puis remettre le fichier à sa base rend le statut de branche propre, mais conserve un changement préparé et un autre non préparé dans le rapport combiné. Un index périmé reste inspectable sur sa base d’origine ; le workspace se compare toujours à ses octets figés. clean exige deux comparaisons propres et un index frais ; périmé seul suffit à rendre faux. Un rapport sale réussi rend 0. --combined et --revision sont exclusifs.

Le scan conserve ignores, fichiers ordinaires et hash BLAKE3 complet ; il n’ingère rien et n’audite pas les payloads stockés.

## Plan de checkout et vérification {#checkout-plan-and-verification}

Le plan compare heads/BRANCH à une révision cible exacte, inventorie le disque et hashe tous les fichiers suivis de la base. Taille/date ne prouvent jamais l’égalité. La politique clean-tracked-v1 exige tous les suivis propres, même inchangés dans la cible. Tout index explicite bloque, même sans différences préparées.

Champs : source/target ; staging_snapshot ; policy/ready ; objects_verified ; executable_metadata_supported (false sur Windows) ; inventory_entries ; hashed_bytes (lecture des suivis, hors audit Store) ; restore_bytes (octets logiques des créations/remplacements) ; counts/actions ; create_directories ; conflicts triés uniques avec path/reason/obstruction. Les transitions de namespace ajoutent namespace_transition et remove_directories.

Actions triées par chemin, avec résumés source/cible (manifest, size, file_hash, executable), côté absent null. Ce sont les changements du snapshot proposé même bloqué, pas un programme ordonné de déplacements. create : nouvel entrant ; replace : contenu différent ; remove : sortie absente de cible ; set_executable : mode seul ; reuse : même contenu/mode, autre manifeste ; unchanged : compteur sans ligne. restore_bytes n’est ni trafic réseau ni nombre unique de blocs ; voir [réutilisation de manifestes](doc:file-comparison). Windows conserve les changements de mode souhaités sans en faire un test de propreté locale.

Par défaut, seules révisions et métadonnées de manifestes nécessaires sont lues, sans blocs cibles ni parents historiques ; un Store partiel de métadonnées peut donc rendre ready true et objects_verified false sans garantir la reconstruction. --verify audite toute la fermeture cible, parents et hashes compris, même si le disque présente des obstacles. Dépendance invalide/absente : erreur sans plan partiel.

Plan prêt : code 0 ; bloqué : JSON et code 1 ; argument, IO ou métadonnée invalide : diagnostic stderr sans JSON.

## Chemins protégés et limites de politique {#protected-paths-and-first-policy-limits}

Le plan n’applique pas .syrnckignore : les ignorés restent visibles pour empêcher écrasement/collision. Les non-suivis sans rapport sont conservés sans hash ; une cible non suivie bloque même si ses octets correspondent.

Motifs : tracked_modified/tracked_missing ; tracked_mode_changed sous Unix ; staging_present/stale_staging ; checkout_pending ; untracked_collision ; file_directory_transition lorsqu’une entrée extérieure à la transition suivie gêne ; path_spelling pour orthographe portable incohérente ; portable_collision pour plusieurs noms physiques équivalents ; symlink_obstruction/special_file ; protected_path pour Store/autre entrée protégée.

Symlinks et points de réanalyse/jonctions Windows sont opaques, jamais parcourus volontairement. .git, .syrnck et le Store réel même personnalisé sont protégés et exclus. Changements propres de casse/normalisation et transitions fichier/dossier passent par l’[exécuteur récupérable](doc:native-checkout#recoverable-namespace-transitions). Seuls fichiers appartenant au snapshot et parents connus peuvent être déplacés ; dossiers vides inconnus et descendants ignorés restent protégés. L’orthographe des parents de chaque snapshot doit être cohérente sous casefold. Aucun force, écrasement, reset d’index ou adoption automatique.

## API C++, progression et ressources {#c-progress-and-resource-boundaries}

Callbacks synchrones non réentrants. scanning compte dossiers/entrées ; verifying les octets du workspace ou de fermeture ; planning les métadonnées/actions, avec compteurs par phase. La planification finit par planning complet, jamais publishing. Annulation/exception ne rendent pas de résultat partiel et ne changent rien. Verrou exclusif et règles normales d’ouverture/récupération restent requis.

Tampon de lecture 1 Mio ; cache FIFO de 1024 résumés de manifeste par chargement de snapshot. Inventaire : budget d’entrées 16 Mio, coût 64 + octets UTF-8 du chemin + chemin normalisé ; profondeur maximale 128. Ce n’est pas la RAM totale : maps, rapports, JSON, allocations et fermetures s’ajoutent. Objets limités à 16 Mio.

L’observation ne verrouille pas le workspace et ne constitue pas un snapshot atomique. Changement observé de taille/type/date/mode autour d’une lecture fait échouer ; une modification ultérieure peut périmer le résultat. L’exécuteur doit donc préparer, revérifier et journaliser, même après ready. Les [sessions C/Go](doc:workspace-abi) exposent l’observation et les statuts ; elles ne fournissent pas toutes les mutations natives.

```cpp
auto status = store.workspace_status(project, "main", operation);
auto preview = store.plan_checkout(project, "main", target_id);
auto verified = store.plan_checkout(project, "main", target_id, true, operation);
```
